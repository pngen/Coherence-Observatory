// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// NVIDIA device backend.
//
// Vendor libraries are loaded dynamically so that the build has no CUDA or
// NVML dependency.  Only genuinely observable facts are published: device
// identity, UUID, memory capacity, driver version, compute capability and the
// measured cost of memory operations this backend actually performs.
//
// Running a CUDA transfer proves nothing about coherence.  Cache-line
// ownership, invalidation counts and peer-coherence transitions are registered
// as UNSUPPORTED unless a vendor API genuinely reports them.

#include "coherence/backends/nvidia.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "coherence/limits.hpp"
#include "text_util.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sol::coherence {
namespace {

constexpr std::string_view kCapabilityDeviceDiscovery = "accelerator.device_discovery";
constexpr std::string_view kCapabilityPeerTopology = "accelerator.peer_topology";
constexpr std::string_view kCapabilityTransferCost = "accelerator.measured_transfer_cost";
constexpr std::string_view kCapabilityCoherence = "accelerator.coherence_telemetry";

/// Minimal dynamic library wrapper.
class DynamicLibrary {
 public:
  DynamicLibrary() = default;
  ~DynamicLibrary() { close(); }
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  bool open(const char* name) {
    close();
#if defined(_WIN32)
    handle_ = reinterpret_cast<void*>(LoadLibraryA(name));
#else
    handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
    return handle_ != nullptr;
  }

  void close() {
    if (handle_ == nullptr) {
      return;
    }
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
  }

  template <class Fn>
  Fn symbol(const char* name) const {
    if (handle_ == nullptr) {
      return nullptr;
    }
#if defined(_WIN32)
    return reinterpret_cast<Fn>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
#else
    return reinterpret_cast<Fn>(dlsym(handle_, name));
#endif
  }

  bool valid() const noexcept { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

// NVML ABI subset actually used.  Declared locally so that no SDK header is
// required and the ABI is explicit.
struct NvmlDevice_st;
using NvmlDevice = NvmlDevice_st*;
using NvmlReturn = int;
constexpr int kNvmlSuccess = 0;

struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

using NvmlInit = NvmlReturn (*)();
using NvmlShutdown = NvmlReturn (*)();
using NvmlSystemGetDriverVersion = NvmlReturn (*)(char*, unsigned int);
using NvmlDeviceGetCount = NvmlReturn (*)(unsigned int*);
using NvmlDeviceGetHandleByIndex = NvmlReturn (*)(unsigned int, NvmlDevice*);
using NvmlDeviceGetName = NvmlReturn (*)(NvmlDevice, char*, unsigned int);
using NvmlDeviceGetUUID = NvmlReturn (*)(NvmlDevice, char*, unsigned int);
using NvmlDeviceGetMemoryInfo = NvmlReturn (*)(NvmlDevice, NvmlMemory*);
using NvmlDeviceGetPciInfo = NvmlReturn (*)(NvmlDevice, void*);
using NvmlDeviceGetCudaComputeCapability = NvmlReturn (*)(NvmlDevice, int*, int*);

// CUDA driver ABI subset.
using CuResult = int;
using CuDevice = int;
using CuContext = void*;
using CuModule = void*;
using CuFunction = void*;
using CuDevicePtr = unsigned long long;

constexpr int kCuSuccess = 0;
struct CuDeviceComputeCapability {
  int major;
  int minor;
};

using CuInit = CuResult (*)(unsigned int);
using CuDriverGetVersion = CuResult (*)(int*);
using CuDeviceGetCount = CuResult (*)(int*);
using CuDeviceGet = CuResult (*)(CuDevice*, int);
using CuDeviceGetName = CuResult (*)(char*, int, CuDevice);
using CuDeviceComputeCapabilityFn = CuResult (*)(int*, int*, CuDevice);
using CuCtxCreate = CuResult (*)(CuContext*, unsigned int, CuDevice);
using CuCtxDestroy = CuResult (*)(CuContext);
using CuCtxSynchronize = CuResult (*)(void);
using CuMemAlloc = CuResult (*)(CuDevicePtr*, unsigned long long);
using CuMemFree = CuResult (*)(CuDevicePtr);
using CuMemcpyHtoD = CuResult (*)(CuDevicePtr, const void*, unsigned long long);
using CuMemcpyDtoH = CuResult (*)(void*, CuDevicePtr, unsigned long long);

/// A trivial PTX kernel: writes a marker into device memory.
constexpr char kPtxSource[] =
    ".version 6.4\n"
    ".target sm_70\n"
    ".address_size 64\n"
    ".visible .entry coherence_marker(.param .u64 out, .param .u32 count)\n"
    "{\n"
    "  .reg .b64 %rd<4>;\n"
    "  .reg .b32 %r<4>;\n"
    "  .reg .pred %p<2>;\n"
    "  ld.param.u64 %rd1, [out];\n"
    "  ld.param.u32 %r1, [count];\n"
    "  mov.u32 %r2, %ctaid.x;\n"
    "  setp.ge.u32 %p1, %r2, %r1;\n"
    "  @%p1 bra done;\n"
    "  mul.wide.u32 %rd2, %r2, 4;\n"
    "  add.s64 %rd3, %rd1, %rd2;\n"
    "  st.global.u32 [%rd3], 0x434f4253;\n"
    "done:\n"
    "  ret;\n"
    "}\n";

using CuModuleLoadData = CuResult (*)(CuModule*, const void*);
using CuModuleGetFunction = CuResult (*)(CuFunction*, CuModule, const char*);
using CuLaunchKernel = CuResult (*)(CuFunction, unsigned int, unsigned int, unsigned int,
                                    unsigned int, unsigned int, unsigned int, unsigned int,
                                    void*, void**, void**);
using CuModuleUnload = CuResult (*)(CuModule);

}  // namespace

NvidiaDiscovery discover_nvidia() {
  NvidiaDiscovery discovery;

  DynamicLibrary nvml;
  if (nvml.open("nvml.dll") || nvml.open("libnvidia-ml.so.1")) {
    const auto init = nvml.symbol<NvmlInit>("nvmlInit_v2");
    const auto get_driver = nvml.symbol<NvmlSystemGetDriverVersion>("nvmlSystemGetDriverVersion");
    const auto get_count = nvml.symbol<NvmlDeviceGetCount>("nvmlDeviceGetCount_v2");
    const auto get_handle =
        nvml.symbol<NvmlDeviceGetHandleByIndex>("nvmlDeviceGetHandleByIndex_v2");
    if (init != nullptr && init() == kNvmlSuccess) {
      discovery.nvml_available = true;
      discovery.mechanisms.push_back("nvmlDeviceGetCount_v2");
      std::array<char, 96> version{};
      if (get_driver != nullptr && get_driver(version.data(), 96u) == kNvmlSuccess) {
        discovery.cuda_driver_version = std::string(version.data());
      }
      unsigned int count = 0;
      if (get_count != nullptr && get_count(&count) == kNvmlSuccess) {
        if (count > Limits::kMaxAcceleratorCount) {
          count = static_cast<unsigned int>(Limits::kMaxAcceleratorCount);
        }
        const auto get_name = nvml.symbol<NvmlDeviceGetName>("nvmlDeviceGetName");
        const auto get_uuid = nvml.symbol<NvmlDeviceGetUUID>("nvmlDeviceGetUUID");
        const auto get_memory = nvml.symbol<NvmlDeviceGetMemoryInfo>("nvmlDeviceGetMemoryInfo");
        const auto get_cc = nvml.symbol<NvmlDeviceGetCudaComputeCapability>(
            "nvmlDeviceGetCudaComputeCapability");
        for (unsigned int index = 0; index < count; ++index) {
          NvmlDevice device = nullptr;
          if (get_handle == nullptr || get_handle(index, &device) != kNvmlSuccess) {
            continue;
          }
          NvidiaDevice info;
          info.index = index;
          if (get_name != nullptr && get_name(device, version.data(), 96u) == kNvmlSuccess) {
            info.name = std::string(version.data());
          }
          if (get_uuid != nullptr && get_uuid(device, version.data(), 96u) == kNvmlSuccess) {
            info.uuid = std::string(version.data());
          }
          NvmlMemory memory{};
          if (get_memory != nullptr && get_memory(device, &memory) == kNvmlSuccess) {
            info.memory_total_bytes = memory.total;
            info.memory_free_bytes = memory.free;
          }
          if (get_cc != nullptr) {
            int major = 0;
            int minor = 0;
            if (get_cc(device, &major, &minor) == kNvmlSuccess) {
              info.compute_capability_major = major;
              info.compute_capability_minor = minor;
            }
          }
          info.driver_version = discovery.cuda_driver_version;
          discovery.devices.push_back(std::move(info));
        }
      }
      discovery.nvml_version = discovery.cuda_driver_version;
    } else {
      discovery.unavailable_reason = "nvmlInit failed";
    }
  } else {
    discovery.unavailable_reason = "nvml library not present";
  }

  DynamicLibrary cuda;
  if (cuda.open("nvcuda.dll") || cuda.open("libcuda.so.1")) {
    const auto driver_version = cuda.symbol<CuDriverGetVersion>("cuDriverGetVersion");
    const auto device_count = cuda.symbol<CuDeviceGetCount>("cuDeviceGetCount");
    const auto init = cuda.symbol<CuInit>("cuInit");
    if (init != nullptr && init(0) == kCuSuccess && driver_version != nullptr &&
        device_count != nullptr) {
      int version = 0;
      if (driver_version(&version) == kCuSuccess) {
        discovery.cuda_driver_version = detail::format_u64(static_cast<std::uint64_t>(version));
      }
      int count = 0;
      if (device_count(&count) == kCuSuccess) {
        discovery.cuda_driver_available = true;
        discovery.mechanisms.push_back("cuDeviceGetCount");
      }
    }
  }
  if (discovery.devices.empty() && discovery.unavailable_reason.empty()) {
    discovery.unavailable_reason = "no NVIDIA device reported by the vendor API";
  }
  return discovery;
}

// ---- CudaWorkload ------------------------------------------------------

namespace {

CudaWorkloadResult run_cuda_workload(std::uint64_t bytes, std::uint32_t device_index) {
  CudaWorkloadResult result;
  DynamicLibrary cuda;
  if (!cuda.open("nvcuda.dll") && !cuda.open("libcuda.so.1")) {
    result.failure_reason = "CUDA driver library is not present";
    return result;
  }
  const auto init = cuda.symbol<CuInit>("cuInit");
  const auto device_get = cuda.symbol<CuDeviceGet>("cuDeviceGet");
  const auto ctx_create = cuda.symbol<CuCtxCreate>("cuCtxCreate_v2");
  const auto ctx_destroy = cuda.symbol<CuCtxDestroy>("cuCtxDestroy_v2");
  const auto ctx_sync = cuda.symbol<CuCtxSynchronize>("cuCtxSynchronize");
  const auto mem_alloc = cuda.symbol<CuMemAlloc>("cuMemAlloc_v2");
  const auto mem_free = cuda.symbol<CuMemFree>("cuMemFree_v2");
  const auto htoD = cuda.symbol<CuMemcpyHtoD>("cuMemcpyHtoD_v2");
  const auto dtoH = cuda.symbol<CuMemcpyDtoH>("cuMemcpyDtoH_v2");
  if (init == nullptr || device_get == nullptr || ctx_create == nullptr || mem_alloc == nullptr ||
      htoD == nullptr || dtoH == nullptr) {
    result.failure_reason = "CUDA driver entry points are unavailable";
    return result;
  }
  if (init(0) != kCuSuccess) {
    result.failure_reason = "cuInit failed";
    return result;
  }
  CuDevice device = 0;
  if (device_get(&device, static_cast<int>(device_index)) != kCuSuccess) {
    result.failure_reason = "cuDeviceGet failed";
    return result;
  }
  CuContext context = nullptr;
  if (ctx_create(&context, 0, device) != kCuSuccess) {
    result.failure_reason = "cuCtxCreate failed";
    return result;
  }
  CuDevicePtr device_buffer = 0;
  if (mem_alloc(&device_buffer, bytes) != kCuSuccess) {
    result.failure_reason = "cuMemAlloc failed";
    if (ctx_destroy != nullptr) {
      ctx_destroy(context);
    }
    return result;
  }

  std::vector<unsigned char> host(static_cast<std::size_t>(bytes));
  for (std::size_t i = 0; i < host.size(); ++i) {
    host[i] = static_cast<unsigned char>((i * 31u) & 0xFFu);
  }

  const auto upload_start = std::chrono::steady_clock::now();
  const bool uploaded = htoD(device_buffer, host.data(), bytes) == kCuSuccess;
  const auto upload_end = std::chrono::steady_clock::now();
  if (!uploaded) {
    result.failure_reason = "cuMemcpyHtoD failed";
    mem_free(device_buffer);
    if (ctx_destroy != nullptr) {
      ctx_destroy(context);
    }
    return result;
  }
  result.bytes_transferred += bytes;
  result.host_to_device_ns =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(upload_end -
                                                                              upload_start)
                              .count());
  result.achieved_host_to_device_bytes_per_second =
      result.host_to_device_ns > 0.0 ? static_cast<double>(bytes) * 1.0e9 / result.host_to_device_ns
                                     : 0.0;
  result.executed = true;

  // A kernel is attempted through the driver API so that no CUDA toolchain is
  // required at build time.  Failure is reported honestly rather than hidden.
  const auto module_load = cuda.symbol<CuModuleLoadData>("cuModuleLoadData");
  const auto get_function = cuda.symbol<CuModuleGetFunction>("cuModuleGetFunction");
  const auto launch = cuda.symbol<CuLaunchKernel>("cuLaunchKernel");
  const auto module_unload = cuda.symbol<CuModuleUnload>("cuModuleUnload");
  if (module_load != nullptr && get_function != nullptr && launch != nullptr) {
    CuModule module = nullptr;
    if (module_load(&module, kPtxSource) == kCuSuccess) {
      CuFunction function = nullptr;
      if (get_function(&function, module, "coherence_marker") == kCuSuccess) {
        unsigned long long argument = device_buffer;
        unsigned int element_count = static_cast<unsigned int>(bytes / 4);
        void* arguments[] = {&argument, &element_count};
        const unsigned int blocks = element_count > 256 ? 256 : element_count;
        const auto kernel_start = std::chrono::steady_clock::now();
        if (launch(function, blocks, 1, 1, 32, 1, 1, 0, nullptr, arguments, nullptr) ==
            kCuSuccess) {
          if (ctx_sync == nullptr || ctx_sync() == kCuSuccess) {
            const auto kernel_end = std::chrono::steady_clock::now();
            result.kernel_ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(kernel_end - kernel_start)
                    .count());
            result.kernel_executed = true;
            result.kernel_launches += 1;
          }
        }
      }
      if (module_unload != nullptr) {
        module_unload(module);
      }
    }
    if (!result.kernel_executed && result.failure_reason.empty()) {
      result.failure_reason =
          "CUDA driver could not load the portable PTX kernel on this device";
    }
  }

  std::vector<unsigned char> downloaded(host.size(), 0);
  const auto download_start = std::chrono::steady_clock::now();
  const bool fetched = dtoH(downloaded.data(), device_buffer, bytes) == kCuSuccess;
  const auto download_end = std::chrono::steady_clock::now();
  if (!fetched) {
    result.failure_reason = "cuMemcpyDtoH failed";
  } else {
    result.bytes_transferred += bytes;
    result.device_to_host_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(download_end - download_start)
            .count());
    result.achieved_device_to_host_bytes_per_second =
        result.device_to_host_ns > 0.0
            ? static_cast<double>(bytes) * 1.0e9 / result.device_to_host_ns
            : 0.0;
    result.parity_verified = std::memcmp(host.data(), downloaded.data(), host.size()) == 0;
  }

  mem_free(device_buffer);
  if (ctx_destroy != nullptr) {
    ctx_destroy(context);
  }
  return result;
}

}  // namespace

// ---- NvidiaBackend -----------------------------------------------------

struct NvidiaBackend::Impl {
  NvidiaConfig config;
  CollectorContext context;
  NvidiaDiscovery discovery;
  CudaWorkloadResult workload;
  bool started = false;
  bool polled = false;
  std::uint64_t emissions = 0;

  Result<Observation> make_observation(EventType type, const std::string& region,
                                       const std::string& source, const std::string& target,
                                       AccessDirection direction, std::uint64_t bytes,
                                       std::int64_t measured_ns, Precision precision,
                                       EvidenceGranularity granularity) {
    Observation observation;
    observation.type = type;
    observation.timestamp_ns = monotonic_now_ns();
    observation.direction = direction;
    observation.bytes = bytes;
    observation.precision = precision;
    observation.granularity = granularity;
    observation.provenance = Provenance::ApplicationInstrumentation;
    observation.measured_duration_ns = measured_ns;
    observation.region = MemoryRegionId{region};
    observation.region_generation = MemoryRegionGeneration{1};
    observation.workload = WorkloadId{"workload.cuda.memory"};
    ResourceRef source_ref;
    source_ref.kind = ResourceKind::Accelerator;
    source_ref.id = ResourceId{source};
    source_ref.generation = 1;
    observation.source = source_ref;
    ResourceRef target_ref;
    target_ref.kind = ResourceKind::MemoryDomain;
    target_ref.id = ResourceId{target};
    target_ref.generation = 1;
    observation.target = target_ref;
    if (context.registrar != nullptr) {
      observation.topology_generation = context.registrar->topology_generation();
    }
    return Result<Observation>(std::move(observation));
  }
};

NvidiaBackend::NvidiaBackend(NvidiaConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

NvidiaBackend::~NvidiaBackend() { stop(); }

std::string_view NvidiaBackend::name() const noexcept { return "nvidia"; }

std::vector<Capability> NvidiaBackend::capabilities() const {
  std::vector<Capability> capabilities;
  auto add = [&capabilities](std::string key, CapabilityStatus status, std::string detail_text,
                             std::string mechanism) {
    Capability capability;
    capability.key = std::move(key);
    capability.status = status;
    capability.detail = std::move(detail_text);
    capability.mechanism = std::move(mechanism);
    capabilities.push_back(std::move(capability));
  };
  add(std::string(kCapabilityDeviceDiscovery),
      impl_->discovery.devices.empty() ? CapabilityStatus::Unsupported : CapabilityStatus::Real,
      impl_->discovery.devices.empty()
          ? "no NVIDIA device was reported by the vendor API"
          : "device identity, UUID, memory capacity and compute capability read from NVML",
      impl_->discovery.nvml_available ? "nvmlDeviceGetCount_v2" : "none");
  add(std::string(kCapabilityPeerTopology), CapabilityStatus::Unsupported,
      impl_->discovery.devices.size() > 1
          ? "peer capability query is available but was not exercised in this run"
          : "fewer than two devices are present, so peer topology cannot be established here",
      "nvmlDeviceGetP2PStatus");
  add(std::string(kCapabilityTransferCost),
      impl_->workload.executed ? CapabilityStatus::Real : CapabilityStatus::Unsupported,
      impl_->workload.executed
          ? "host-to-device and device-to-host transfer cost measured by this backend"
          : "no CUDA memory operation was performed",
      "cuMemcpyHtoD_v2/cuMemcpyDtoH_v2");
  add(std::string(kCapabilityCoherence), CapabilityStatus::Unsupported,
      "no vendor interface on this host exposes accelerator cache-line ownership, invalidation "
      "counts or peer-coherence transitions; executing CUDA transfers does not reveal them",
      "none");
  return capabilities;
}

Status NvidiaBackend::start(const CollectorContext& context) {
  if (context.sink == nullptr) {
    return fail(ErrorCode::InvalidArgument, "nvidia backend requires an ingestion sink");
  }
  impl_->context = context;
  impl_->discovery = discover_nvidia();
  if (impl_->config.run_workload && !impl_->discovery.devices.empty()) {
    const std::uint64_t bytes =
        impl_->config.workload_bytes == 0 ? (8ull << 20) : impl_->config.workload_bytes;
    impl_->workload = run_cuda_workload(bytes, impl_->config.device_index);
  }

  StructureRegistrar* registrar = context.registrar;
  if (registrar != nullptr && !impl_->discovery.devices.empty()) {
    CoherenceDomainRecord domain;
    domain.id = CoherenceDomainId{"cd.accelerator.observed"};
    domain.generation = CoherenceDomainGeneration{1};
    domain.protocol_family = "not-observable";
    registrar->register_coherence_domain(domain);
    for (const NvidiaDevice& device : impl_->discovery.devices) {
      const std::string suffix = detail::format_u64(device.index);
      MemoryDomainRecord memory;
      memory.id = MemoryDomainId{"md.accel." + suffix};
      memory.generation = MemoryDomainGeneration{1};
      memory.node = NodeId{"node.host.0"};
      memory.kind = MemoryDomainKind::DeviceMemory;
      memory.coherence_domain = CoherenceDomainId{"cd.accelerator.observed"};
      memory.coherent_with_host = false;
      memory.coherent_with_host_known = false;
      memory.capacity_bytes = device.memory_total_bytes;
      memory.capacity_known = device.memory_total_bytes != 0;
      registrar->register_memory_domain(memory);

      AcceleratorRecord accelerator;
      accelerator.id = AcceleratorId{"acc.nvidia." + suffix};
      accelerator.node = NodeId{"node.host.0"};
      accelerator.generation = DeviceGeneration{1};
      accelerator.vendor = "nvidia";
      accelerator.vendor_uuid = device.uuid;
      accelerator.local_memory_domain = memory.id;
      accelerator.peer_coherent_capability_known = false;
      registrar->register_accelerator(accelerator);

      RegionRecord region;
      region.id = MemoryRegionId{"region.accel." + suffix};
      region.generation = MemoryRegionGeneration{1};
      region.owner = "nvidia-backend";
      region.memory_domain = memory.id;
      region.memory_domain_generation = MemoryDomainGeneration{1};
      region.coherence_domain = CoherenceDomainId{"cd.accelerator.observed"};
      region.sharing_scope = SharingScope::DeviceShared;
      region.size_bytes = device.memory_total_bytes;
      region.size_known = device.memory_total_bytes != 0;
      region.page_size_bytes = 65536;
      region.page_size_known = true;
      region.allocation_generation = 1;
      region.mapping_generation = 1;
      region.workload = WorkloadId{"workload.cuda.memory"};
      region.annotation = device.name;
      registrar->register_region(region);

      TopologyLink link;
      link.from.kind = ResourceKind::Accelerator;
      link.from.id = ResourceId{accelerator.id.str()};
      link.from.generation = 1;
      link.to.kind = ResourceKind::MemoryDomain;
      link.to.id = ResourceId{memory.id.str()};
      link.to.generation = 1;
      link.locality = Locality::LocalAccelerator;
      link.provenance = Provenance::VendorTelemetryApi;
      registrar->set_topology_link(link);
    }
    for (const Capability& capability : capabilities()) {
      registrar->set_capability(capability);
    }
  }
  impl_->started = true;
  return Status();
}

Status NvidiaBackend::poll(const CollectorContext& context) {
  if (!impl_->started) {
    return fail(ErrorCode::Busy, "nvidia backend is not started");
  }
  if (impl_->polled) {
    return Status();
  }
  impl_->polled = true;
  if (!impl_->workload.executed) {
    return Status();
  }
  const std::string suffix = detail::format_u64(impl_->config.device_index);
  Result<Observation> read = impl_->make_observation(
      EventType::RemoteRead, "region.accel." + suffix, "acc.nvidia." + suffix,
      "md.accel." + suffix, AccessDirection::Read, impl_->workload.bytes_transferred / 2,
      static_cast<std::int64_t>(impl_->workload.host_to_device_ns), Precision::Derived,
      EvidenceGranularity::Region);
  Result<Observation> write = impl_->make_observation(
      EventType::RemoteWrite, "region.accel." + suffix, "acc.nvidia." + suffix,
      "md.accel." + suffix, AccessDirection::Write, impl_->workload.bytes_transferred / 2,
      static_cast<std::int64_t>(impl_->workload.device_to_host_ns), Precision::Derived,
      EvidenceGranularity::Region);
  if (read.ok()) {
    const Result<IngestionOutcome> outcome = context.sink->publish(read.take());
    if (outcome.ok()) {
      ++impl_->emissions;
    }
  }
  if (write.ok()) {
    const Result<IngestionOutcome> outcome = context.sink->publish(write.take());
    if (outcome.ok()) {
      ++impl_->emissions;
    }
  }
  return Status();
}

Status NvidiaBackend::stop() {
  impl_->started = false;
  return Status();
}

const NvidiaDiscovery& NvidiaBackend::discovery() const noexcept { return impl_->discovery; }
const CudaWorkloadResult& NvidiaBackend::workload() const noexcept { return impl_->workload; }

}  // namespace sol::coherence
