// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/cost.hpp"

#include "coherence/checked.hpp"

namespace sol::coherence {
namespace {

Status validate_coefficient(const char* name, double value, double lower, double upper) {
  if (!finite_within(value, upper) || value < lower) {
    return fail(ErrorCode::OutOfRange, std::string("cost coefficient out of range: ") + name);
  }
  return Status();
}

}  // namespace

std::string_view to_string(CostKind kind) noexcept {
  switch (kind) {
    case CostKind::Measured: return "MEASURED";
    case CostKind::CounterDerived: return "COUNTER_DERIVED";
    case CostKind::ModelEstimated: return "MODEL_ESTIMATED";
    case CostKind::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

Status CostModel::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "cost model id must not be empty");
  }
  if (id.size() > 96) {
    return fail(ErrorCode::TooLarge, "cost model id too long");
  }
  Status s = validate_coefficient("remote_read_latency_ns", remote_read_latency_ns, 0.0, 1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("remote_write_latency_ns", remote_write_latency_ns, 0.0, 1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("invalidation_latency_ns", invalidation_latency_ns, 0.0, 1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("ownership_transfer_latency_ns", ownership_transfer_latency_ns, 0.0,
                           1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("retry_latency_ns", retry_latency_ns, 0.0, 1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("conflict_stall_ns", conflict_stall_ns, 0.0, 1.0e12);
  if (!s.ok()) return s;
  s = validate_coefficient("bandwidth_bytes_per_ns", bandwidth_bytes_per_ns, 0.0, 1.0e9);
  if (!s.ok()) return s;
  s = validate_coefficient("cxl_multiplier", cxl_multiplier, 0.0, 1.0e6);
  if (!s.ok()) return s;
  s = validate_coefficient("pooled_multiplier", pooled_multiplier, 0.0, 1.0e6);
  if (!s.ok()) return s;
  s = validate_coefficient("remote_node_multiplier", remote_node_multiplier, 0.0, 1.0e6);
  if (!s.ok()) return s;
  if (version == 0) {
    return fail(ErrorCode::InvalidArgument, "cost model version must be non-zero");
  }
  return Status();
}

CostModel default_cost_model() {
  CostModel model;
  model.id = "generic-numa-latency";
  model.version = 1;
  model.defined = true;
  // Documented, illustrative defaults.  These are NOT measurements from this
  // host; they exist so that a harness without a calibration still produces a
  // labelled MODEL_ESTIMATED figure rather than a fabricated one.
  model.remote_read_latency_ns = 180.0;
  model.remote_write_latency_ns = 220.0;
  model.invalidation_latency_ns = 90.0;
  model.ownership_transfer_latency_ns = 240.0;
  model.retry_latency_ns = 400.0;
  model.conflict_stall_ns = 600.0;
  model.bandwidth_bytes_per_ns = 12.0;
  model.cxl_multiplier = 1.8;
  model.pooled_multiplier = 2.1;
  model.remote_node_multiplier = 3.0;
  return model;
}

}  // namespace sol::coherence
