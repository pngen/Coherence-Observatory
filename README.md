# Coherence Observatory

**Version 1.0.0** -- Copyright 2026 Summon Software Labs. Apache License 2.0.

A production-grade, open-source, vendor-neutral **C++20** runtime for observing,
attributing, correlating, explaining and preserving evidence about coherence
behaviour across heterogeneous accelerator, CPU, host-memory, pooled-memory,
CXL-class and distributed memory infrastructure.

No third-party dependencies. C++20 and the operating system only.

---

## What Coherence Observatory is

Coherence Observatory answers exactly one question:

> **What coherence-related activity is occurring now, where is it occurring,
> which resources and generations caused it, how much does it cost, what
> evidence supports that conclusion, and how certain is the attribution?**

It exists to hold a line that most performance tooling blurs:

> **A counter increased.**
>
> versus
>
> **This exact current resource generation produced this exact class of
> coherence activity under this topology, ownership, locality and evidence
> state, and the runtime can explain the attribution and cost.**

The runtime is observational. It does not own coherence policy. It does not
arbitrate ownership. It does not repair coherence. It does not issue
invalidations. It does not migrate data because remote access is expensive. It
does not decide consistency semantics for another runtime. It explains and
preserves evidence about those systems.

## Exact boundary

**Owned by Coherence Observatory**

Coherence-event ingestion; resource identity; memory-region identity where
observable; producer/publisher identity; evidence provenance; evidence
freshness; event generations; topology-generation binding; locality
classification; coherence-state observations; ownership-transition
observations; invalidations; downgrades/upgrades; remote accesses; remote reads
and writes; cache-line or region movement where observable;
directory/snoop-related events where observable; retry/replay events where
coherence-relevant; cross-domain consistency transitions; memory-domain
transitions; coherence traffic accounting; attribution; correlation;
aggregation; cost estimation from explicitly supplied or measured models;
hotspot identification; false-sharing-like pattern detection where supportable;
ping-pong ownership detection; read/write-sharing analysis; cross-NUMA,
cross-device and CXL-class remote-access analysis; deterministic explanations;
immutable snapshots; durable structural state; conservative recovery;
multiprocess publisher authority where distributed; REAL / SYNTHETIC /
UNSUPPORTED provenance.

**Not owned by Coherence Observatory**

Coherence protocol implementation; cache controller behaviour; directory
control; memory allocation; memory placement; memory migration; scheduling;
accelerator partitioning; general topology ownership; CXL pooling; accelerator
virtualization; resource brokering; workload admission; generic performance
tracing unrelated to coherence; generic hardware monitoring; memory-pressure
control; power/thermal policy; kernel execution policy; consistency semantics
belonging to Coherence Fabric; automatic remediation.

It may consume identities, topology, memory-domain information or coherence
semantics from adjacent runtimes. It does not absorb them.

## Relationship to Coherence Fabric

Coherence Fabric governs coherence and consistency semantics. Coherence
Observatory explains what actually happened. The distinction is mandatory and is
enforced by the type system: nothing in this repository can issue an
invalidation, change ownership, migrate memory or alter a consistency rule.

Coherence Fabric may say:

> This region should be owned here under this consistency generation.

Coherence Observatory may observe:

> The region changed ownership 41 times, generated 18 invalidation bursts,
> incurred remote-access traffic across two domains, and violated the expected
> locality pattern.

Governance and observation are not merged.

## Architectural doctrine

The runtime makes these distinctions explicit and testable:

* Observed is not inferred.
* Inferred is not proven.
* A hardware counter is not automatically attributable to one memory region.
* A process event is not automatically current after restart.
* A sample from one generation must not be assigned to a replacement device or
  memory region merely because an identifier string matches.
* `UNKNOWN`, `UNATTRIBUTED` and `AMBIGUOUS` are first-class outcomes.
* `SYNTHETIC` is never `REAL`.
* A coarse counter is never presented as exact per-region evidence.
* Precision is never manufactured from insufficient observability.

## Observation model

Observations are immutable evidence records (`coherence::Observation`). Each one
can carry:

event identity; event type; source publisher; publisher boot incarnation;
source device/domain; target device/domain; memory region where known; region
generation; coherence-domain identity and generation; topology generation;
timestamp or monotonic sequence; counter delta where applicable; byte count;
line/page/region count; access direction; ownership before/after if known; state
before/after if known; locality classification; provenance; precision;
REAL/SYNTHETIC status; bounded raw backend metadata; evidence generation;
sampling epoch.

Once accepted, an observation is never mutated. Raw backend metadata is bounded,
key-unique and explicitly typed, and it is never read as authority by the
analysis path.

Structural validation (`validate_observation_structure`) rejects, before any
mutation: zero event identity; empty or malformed publisher identity; a
region reference without a non-zero generation; a region generation without a
region; source and target naming the same resource; a node reference carrying a
device generation; timestamps or byte counts outside their bounds; exact
precision without a stated evidence granularity; a counter delta claiming
exact-event precision; a counter delta without a counter generation; a locality
class without the declared flag; and an unmappable backend event that does not
retain its backend-native name.

## Event taxonomy

```
REMOTE_READ              REMOTE_WRITE             INVALIDATION
OWNERSHIP_TRANSFER       OWNERSHIP_UPGRADE        OWNERSHIP_DOWNGRADE
SHARED_READ              WRITE_EXCLUSIVE_TRANSITION
CACHE_TO_CACHE_TRANSFER  MEMORY_DOMAIN_TRANSFER
SNOOP_REQUEST            SNOOP_RESPONSE
DIRECTORY_LOOKUP         DIRECTORY_MISS
COHERENCE_RETRY          COHERENCE_CONFLICT
WRITEBACK                REMOTE_ATOMIC
CONSISTENCY_BARRIER      FLUSH                    FENCE
REGION_INVALIDATION      UNKNOWN_COHERENCE_EVENT
```

Every class listed is implemented: each has a deterministic aggregation bucket,
an attribution rule and, where the evidence supports it, an analyzer that
consumes it. Backend-native events map onto this taxonomy conservatively. When a
backend event has no exact generic equivalent it must be published as
`UNKNOWN_COHERENCE_EVENT` with the backend-native name in the bounded
metadata; structural validation refuses such an event if the backend detail is
missing, so an unmappable event can never lose its identity.

## Precision classes

```
EXACT_EVENT > EXACT_COUNTER_DELTA > SAMPLED_EVENT > AGGREGATED_COUNTER
            > DERIVED > INFERRED > UNKNOWN
```

The ordering is total and is used everywhere: aggregation keeps the weakest
precision of its inputs plus per-precision counts, attribution may only weaken
precision, and a region-level claim requires evidence whose granularity
actually resolves a region.

Evidence granularity is separate and explicit:
`CACHE_LINE > PAGE > REGION > DEVICE > DOMAIN > AGGREGATE > UNKNOWN`.

Examples enforced by tests:

* A socket-wide counter is published with `CounterScope::PerDevice` and
  `GRANULARITY::DEVICE`; the runtime refuses it if it names a region, and files
  the resulting attribution as `ATTRIBUTED_AGGREGATE_ONLY`.
* An OS-level page sample is published with granularity `PAGE` and precision
  `SAMPLED_EVENT`; it can never be reported as exact cache-line ownership.
* A synthetic backend emitting exact region transitions publishes
  `EXACT_EVENT` evidence whose reality class is `SYNTHETIC`.

## Provenance

```
HARDWARE_PERFORMANCE_COUNTER   VENDOR_TELEMETRY_API   OS_TELEMETRY
RUNTIME_INSTRUMENTATION        APPLICATION_INSTRUMENTATION
FABRIC_PROVIDED_STATE          SYNTHETIC_BACKEND      IMPORTED_TRACE
DERIVED_AGGREGATION            UNKNOWN
```

Reality is derived, never asserted:

* `HARDWARE_PERFORMANCE_COUNTER`, `VENDOR_TELEMETRY_API`, `OS_TELEMETRY`,
  `RUNTIME_INSTRUMENTATION`, `APPLICATION_INSTRUMENTATION` and
  `FABRIC_PROVIDED_STATE` are `REAL`.
* `SYNTHETIC_BACKEND` is `SYNTHETIC`.
* `IMPORTED_TRACE`, `DERIVED_AGGREGATION` and `UNKNOWN` are `MIXED`: they
  are never promoted to `REAL`.
* Mixed contributions to one aggregate bucket produce `MIXED`, and the bucket
  keeps per-provenance counts so the composition is inspectable.

Every derived result retains references to the observations that support it
(`EvidenceRef`: event identity, publisher, publisher boot, sequence, event
class, precision, provenance).

## Publisher authority

Where publishers run in separate processes, evidence is bound to publisher
identity, publisher boot identity, coordinator epoch, device generation,
memory-domain generation, region generation, evidence generation and topology
generation.

* A dead publisher permanently loses authority for its boot identity. The
  coordinator detects loss through the real control path: the transport
  connection ends, and the publisher boot is fenced with reason
  `CONNECTION_CLOSED`.
* A replacement publisher must register under a fresh boot identity. A fenced
  boot identity is never re-admitted, and a superseded boot is refused with
  `STALE_BOOT` for the rest of the coordinator incarnation.
* Stale frames are rejected: epoch mismatch (`STALE_EPOCH`), boot mismatch
  (`STALE_BOOT`), missing evidence generation and sequence regression are all
  refused before any mutation.
* Persisted publisher liveness never becomes current after restart: durable
  recovery restores only replay watermarks, and every publisher must
  re-register and republish before its evidence counts.

## Ordering

Transport arrival order is not event order. Each publisher boot has its own
sequence space with deterministic outcomes:

| Situation | Outcome |
| --- | --- |
| First sequence | accepted |
| Strictly increasing, gap of N | accepted, gap of N recorded as missing evidence |
| Inside the retention window, never seen | accepted, flagged `late` |
| Inside the retention window, identical content | `DUPLICATE` -- idempotent, not counted twice |
| Inside the retention window, different content for the same sequence or event identity | `CONFLICT` -- rejected, nothing mutated |
| At or below the retention window floor | `STALE_SEQUENCE` -- rejected, nothing mutated |
| At or below a watermark restored from durable state | `STALE_SEQUENCE` -- a replay cannot be distinguished from a re-send, so it is refused |

Counter reset, counter wrap, counter-generation change, device reset,
sampling-epoch change and publisher restart are each classified explicitly and
never silently bridged.

## Counter semantics

Modelled kinds: `ABSOLUTE`, `DELTA`, `RESETTABLE`, `WRAPPING`, `SAMPLED`.
Modelled scopes: `PER_REGION`, `PER_DEVICE`, `PER_DOMAIN`, `PER_LINK`,
`PER_PROCESS`, `GLOBAL`.

* Only a genuinely per-region counter may name a region; any other scope that
  names one is rejected.
* A counter whose scope is coarser than `PER_REGION` may not claim evidence
  granularity finer than `DEVICE`.
* The first sample of an absolute counter establishes a baseline and yields no
  delta: prior traffic is not attributed to this interval.
* A decrease is a reset unless the counter is declared `WRAPPING` with a width
  below 64 bits and the decrease exceeds half the range. **A reset never produces
  negative traffic and never fabricates positive traffic.**
* A 64-bit counter decrease is always treated as a reset, because a wrap cannot
  be distinguished from one.
* A counter-generation or descriptor change breaks delta continuity: the
  accumulated total is reset and the discontinuity is recorded.
* Every delta becomes a first-class observation with
  `EXACT_COUNTER_DELTA` precision, so counter evidence flows through exactly
  the same attribution, aggregation and analysis pipeline as event evidence.
* Accumulation uses checked arithmetic; saturation is flagged and reported.
* A counter value beyond its declared width is rejected.

## Region model

`RegionRecord` carries id, generation, opaque handle, owner/runtime source,
memory domain and its generation, coherence domain, sharing scope, size, page
size, allocation generation, mapping generation, workload annotation and
retirement state.

Raw process virtual addresses are never stored, serialized or reported.
Callers holding only a pointer use `make_opaque_handle`, which mixes the
address with a process-local salt and discards it; the resulting handle is not
reversible without the salt. Region identity is the opaque identity plus the
generation, never the address.

A region retirement must name the exact current generation; a mismatched
generation is refused. Re-registering a region at an older generation is
refused as a rollback. Evidence bound to a superseded or retired generation is
recorded as stale evidence and never counted.

## Locality

```
LOCAL_PROCESSOR  LOCAL_NUMA  REMOTE_NUMA  LOCAL_ACCELERATOR  PEER_ACCELERATOR
SAME_ROOT_COMPLEX  REMOTE_ACCELERATOR  CXL_ATTACHED  POOLED_MEMORY  REMOTE_NODE
UNKNOWN
```

Locality is never inferred from device presence. It is either declared by the
evidence source, validated against the topology generation the evidence was
produced under, or derived from a registered topology link at that same
generation. A conflict between declared and topology-derived locality is
reported as ambiguity with both candidates listed, never silently resolved.

Topology is bound to a generation. Every topology mutation advances it.
Evidence bound to a superseded topology generation is reported as
`STALE_EVIDENCE`; the runtime never silently re-binds old evidence to a new
topology.

## Attribution

Attribution determines what can safely be attributed to a resource, device,
memory domain, region, workload, process, coherence domain, topology edge,
locality class or event class -- and states what it was attributed to, from
which evidence, with which precision and provenance, under which generation
bindings, and whether the result is exact, partial, ambiguous or impossible.

Outcomes:

```
ATTRIBUTED_EXACT           bound to a specific region generation with evidence
                           that resolves it
ATTRIBUTED_PARTIAL         bound to a resource/domain/workload, not to a region
ATTRIBUTED_AGGREGATE_ONLY  only an aggregate bucket is defensible
AMBIGUOUS                  several mutually exclusive subjects remain possible
UNATTRIBUTED               no subject could be bound
STALE_EVIDENCE             superseded before it could be attributed
UNSUPPORTED                the claim needs a capability this host does not have
```

Stable reason codes accompany every outcome (`bound.region_generation`,
`limited.evidence_granularity`, `stale.region_generation`,
`stale.topology_generation`, `ambiguous.multiple_regions`,
`unsupported.capability`, `unattributed.no_subject`, ...). The engine never forces uncertain evidence into
an exact bucket, and the invariant "attribution precision never exceeds evidence
precision" is enforced structurally and tested exhaustively.

Capability gating is applied during attribution: a REAL-provenance observation
that claims cache-line granularity is classified `UNSUPPORTED` unless a REAL
cache-line capability is registered, and CXL/pooled locality cannot be claimed
by real telemetry on a host with no CXL telemetry.

## Cost model

Cost models are explicit, versioned data with named coefficients
(`CostModel`: id, version, per-remote-read/write latency, per-invalidation
latency, per-ownership-transfer latency, retry latency, conflict stall,
effective bandwidth, CXL/pooled/remote-node multipliers).

Every figure distinguishes:

```
MEASURED          read from a real measurement supplied by the source
COUNTER_DERIVED   computed from exact counter deltas
MODEL_ESTIMATED   produced by applying the named model to observed counts
UNKNOWN           not estimable from the available evidence
```

Each `CostEstimate` carries its full decomposition as named `CostTerm`s, each
with its own kind, coefficient, observed quantity and unit, plus the model id and
version. The default model is `generic-numa-latency` version 1: documented,
illustrative coefficients that are **not** measurements from this host. A
measured latency is never presented as an estimate, and an estimate is never
presented as measured.

## Aggregation

Exact bounded aggregation across ten dimensions: event class, device, domain,
region, workload, locality, time bucket, publisher, coherence domain and
source-to-target pair.

All accumulation uses checked arithmetic; overflow saturates and sets an
explicit flag that propagates to the snapshot, the CLI and the findings. A
bucket that cannot be created because the store is at capacity refuses the
update and records the loss. Buckets are ordered, so rendering and iteration are
deterministic. Each bucket keeps the weakest precision among its inputs,
per-precision and per-provenance counts, the observation window, and its
REAL/SYNTHETIC/MIXED reality.

## Hotspot detection

Hotspots are produced with named reasons and decomposed metrics; there are no
opaque hotness scores. Signals: high remote-read rate, high remote-write rate,
repeated ownership transfer, invalidation bursts, region-level contention,
disproportionate coherence bytes, cross-NUMA traffic, cross-device traffic,
cross-domain traffic, CXL-class traffic, and source-to-target concentration.
Every threshold lives in `HotspotPolicy` as named, versioned data, and every
finding states whether its threshold was exceeded.

## Ping-pong detection

An explicit pattern: the same region, directional alternation between
ownership-transfer events, at least `min_alternations` alternations inside a
time window, no gap larger than `max_gap_ns`, at least two distinct
participants, and evidence at or above the configured minimum precision.

The result reports participants, region, alternation count, the direction
sequence, supporting evidence references, precision, provenance, reality and
cost. Aggregate counters cannot establish ping-pong; a test proves that
aggregate-precision alternation evidence produces no finding.

## False-sharing-like analysis

The classification always names the evidence granularity:

```
CACHE_LINE_FALSE_SHARING_SUPPORTED   line-granularity evidence with explicit
                                     line identity and multiple writers
REGION_CONTENTION_LIKELY             region-granularity evidence showing
                                     contention
PAGE_LEVEL_CONTENTION                page-granularity evidence only
INSUFFICIENT_GRANULARITY             the evidence cannot say
```

Line-level false sharing is claimed only when the evidence genuinely resolves a
cache line *and* carries an explicit line identity in the bounded metadata key
`line.index`. Line-granularity evidence without line identity is downgraded to
`INSUFFICIENT_GRANULARITY` with a stated missing-evidence entry rather than
guessed at.

## Invalidation analysis

Counts, rates and bursts per source, target and region, with the coherence
states around each invalidation, the region generation, the evidence
granularity and the estimated cost. Individual invalidation events and
aggregate invalidation counters are counted separately and never mixed: an
aggregate counter produces its own finding labelled
`INSUFFICIENT_GRANULARITY` that states it carries no source, target or region
identity.

## Remote-access analysis

Rows keyed by source domain, target domain and region generation, each with
locality class, access direction, count, bytes, read/write split, cost
decomposition, cost kind, topology generation, precision, provenance and
reality. A row built from counter evidence is marked `aggregate_only`.

Observation and interpretation stay separate: remote access is reported as
remote access, and nothing in the runtime asserts that a remote access is a
coherence problem.

## Explanation engine

Every high-level result carries a structured `Explanation`: finding, evidence,
relevant identities, relevant generations, provenance, precision, decisive
metrics, missing evidence, ambiguity, the cost model used and whether the finding
is REAL or SYNTHETIC. Explanations are data, not prose; `render_text` and
`render_json` are deterministic, and identical inputs produce byte-identical
output. Analysis output is sorted by a content-derived ordering key, so stable
inputs yield stable finding order -- proven by test.

## Snapshots

`Observatory::snapshot()` returns `std::shared_ptr<const Snapshot>`: an
immutable value that states the coordinator epoch, observation epoch, topology
generation, snapshot generation and capture time, and that exposes publishers,
nodes, processors, accelerators, memory domains, coherence domains, regions,
topology, aggregates, findings, stale evidence, capabilities, loss accounting,
the cost model, historical aggregates and historical evidence summaries.

`Snapshot::is_current(coordinator, observation)` makes currentness explicit: a
snapshot taken before a generation change cannot masquerade as current.

> **Lifetime note.** The containers are reached through the snapshot handle.
> Bind the handle to a named local before iterating, for example
> `const auto snapshot = observatory.snapshot(); for (const auto& region :
> snapshot->regions()) ...`. Iterating a temporary
> (`observatory.snapshot()->regions()`) leaves the range expression referring
> into a destroyed temporary under the C++20 range-for rules.

## Persistence and recovery

Durable state is written to a versioned, little-endian, length-bounded,
integrity-checked file:

* 64-byte header (magic `COBSST01`, version, flags, payload length, coordinator
  and observation epochs, record count, header CRC-32),
* a sequence of typed sections, each with its own length and CRC-32,
* a 16-byte trailer (magic `COBSEND1`, payload CRC-32).

Sections carry: observer identity, nodes, processors, accelerators, memory
domains, coherence domains, regions, topology links, publisher replay
watermarks (including live publishers' watermarks), capability classification,
the cost model, bounded historical aggregates and bounded historical evidence.

Only durable state is persisted. Dynamic liveness and freshness are never
persisted as current.

Loading parses and validates the entire file into staging storage -- magic,
version, flags, header CRC, payload CRC, section CRCs, exact sizes, duplicate
sections, duplicate identities, enum ranges, non-zero generations, referential
integrity (region-to-domain, domain-to-coherence-domain, processor-to-node),
declared record counts -- and only then applies it. **A failed load cannot
partially apply**, which is verified by comparing state fingerprints before and
after every corruption case.

Recovery is conservative:

* the coordinator epoch advances, so old-epoch frames are rejected;
* the observation epoch advances;
* structure is restored and marked as loaded;
* publishers are **not** current and must re-register and republish;
* prior-run activity is restored as history and never enters current aggregates;
* replay watermarks are preserved, so re-sending an already-accepted sequence is
  refused rather than accepted as "late";
* current aggregates, the observation journal, per-region samples, counters and
  stale records start empty.

Retention is bounded: history records, historical aggregates, publisher
watermarks, the observation journal and per-region sample windows all have
explicit caps, and eviction is counted as evidence loss.

## Distributed behaviour

```
ObservatoryCoordinator  (CoordinatorServer)   authoritative state + real framed TCP
ObservationPublisher                          a producing process, own identity
ObservationClient                             read-only queries
ObservatoryAdminClient                        structural mutation only
collectors                                    synthetic / cpu-os / nvidia / trace
state store                                   versioned durable state
attribution engine, analyzers, CLI            in-process
```

Publisher death and coordinator restart are proven with **real independent
operating-system processes** and **real TCP sockets**:

* `cohobsd` runs the coordinator in its own process;
* `cohobs-publisher` runs each publisher in its own process;
* the death test performs a real `TerminateProcess`/`SIGKILL` kill, observes the
  fencing through the coordinator's query protocol, proves the surviving
  publisher is unaffected, proves the fenced boot cannot be re-admitted, proves a
  fresh boot is accepted, and proves that the replacement's evidence only
  becomes current after fresh publication;
* the restart test kills the coordinator process, starts a fresh one against the
  same durable state, proves the epoch advanced, proves no pre-restart evidence
  is current, proves the previous epoch is refused, proves the same boot resumes
  only from its persisted watermark, and proves a replay below that watermark is
  rejected while fresh sequences are accepted.

Threads are never called multiprocess: the coordinator's I/O loop is a polling
loop over real sockets, and every publisher is a separate executable.

## Framed protocol

```
Header (32 bytes)
  char[8] magic "COBSFRM1"     u16 version       u16 type
  u32 flags                    u64 frame_seq     u32 payload_length
  u32 header_crc  (CRC-32 over the header with this field zeroed)
payload
u32 payload_crc (CRC-32 over the payload)
```

Bounded and versioned, with payload length capped at 1 MiB. Messages:
`HELLO`, `HELLO_ACK`, `REGISTER_PUBLISHER`, `REGISTER_RESOURCE`,
`REGISTER_REGION`, `REGISTER_COHERENCE_DOMAIN`, `SET_TOPOLOGY_LINK`,
`PUBLISH_EVENT`, `PUBLISH_BATCH`, `PUBLISH_COUNTER`, `QUERY_SNAPSHOT`,
`QUERY_FINDINGS`, `QUERY_ATTRIBUTION`, `FENCE`, `HEARTBEAT`, `SAVE_STATE`,
`BUMP_TOPOLOGY`, `RETIRE_REGION`, `ACK`, `ERROR`, `SNAPSHOT_RESPONSE`,
`FINDINGS_RESPONSE`, `ATTRIBUTION_RESPONSE`.

Authority is role-based. A `PUBLISHER` connection may register itself and
publish its own evidence; a `CLIENT` connection may query; only an `ADMIN`
connection may register resources or regions, set topology, fence a publisher,
retire a region, advance the topology generation or request a state save. A role
that sends a message outside its authority is refused and its connection closed.

Rejected, with nothing mutated: bad magic, unsupported version, unknown type,
invalid flags, oversized payload, truncation, bad integrity, bytes trailing a
declared frame, duplicate HELLO, messages before HELLO, stale epoch, stale boot,
stale generation, sequence regression, malformed payloads, malformed event
trees, invalid batch counts and unauthorized structural publication.

A snapshot response larger than the frame bound is a serialization failure: the
coordinator sends an error rather than a truncated payload. Snapshots carried
over the wire hold a bounded prefix of the aggregate buckets, and the snapshot
reports both the total held and the number omitted.

## REAL / SYNTHETIC / UNSUPPORTED semantics

Every hardware-facing capability is classified on the host where the runtime is
running, with the mechanism that justified the classification:

* **REAL** -- genuinely observable here and actually observed.
* **SYNTHETIC** -- not observable here; the semantics are proven by the
  deterministic synthetic backend, which publishes through the *same* ingestion,
  validation, aggregation, attribution and analysis pipeline.
* **UNSUPPORTED** -- not observable here and not simulated.

A synthetic backend cannot relabel its evidence as REAL: reality is derived from
provenance, mixed contributions become MIXED, and a REAL-provenance claim that
needs an unsupported capability is classified `UNSUPPORTED` at attribution time.

## Hardware validation

Validated on this machine: Windows 11 Pro (build 26200) x64, AMD Ryzen 7 9800X3D
(8 cores / 16 logical processors, **single NUMA node**), 61.6 GiB RAM,
NVIDIA GeForce RTX 5090 (driver 32.0.16.1692), CUDA driver 12.9/13.1 present,
MSVC 19.44.

Observed classification on this host:

| Capability | Status | Mechanism |
| --- | --- | --- |
| `cpu.topology` | REAL | `GetLogicalProcessorInformationEx` |
| `cpu.numa_topology` | REAL | `GetNumaHighestNodeNumber`, `GetNumaNodeProcessorMaskEx` |
| `cpu.cache_geometry` | REAL | `GetLogicalProcessorInformationEx` |
| `os.page_numa_placement` | REAL | `QueryWorkingSetEx` |
| `accelerator.device_discovery` | REAL | NVML `nvmlDeviceGetCount_v2` and friends |
| `accelerator.measured_transfer_cost` | REAL | CUDA driver `cuMemcpyHtoD_v2`/`cuMemcpyDtoH_v2` |
| `cpu.remote_numa_observability` | UNSUPPORTED | single NUMA node; semantics proven synthetically |
| `accelerator.peer_topology` | UNSUPPORTED | fewer than two devices present |
| `coherence.cache_line_ownership` | UNSUPPORTED | no OS or vendor interface exposes it |
| `coherence.invalidation_counters` | UNSUPPORTED | no interface reports invalidations |
| `accelerator.coherence_telemetry` | UNSUPPORTED | no vendor interface exposes GPU coherence |
| `memory.cxl_telemetry` | UNSUPPORTED | no CXL device or telemetry interface detected |
| `memory.pooled_telemetry` | UNSUPPORTED | no pooled-memory fabric telemetry detected |

The CPU/OS backend runs a real multi-threaded read and write workload over a real
allocation and publishes REAL measurements of it (provenance
`APPLICATION_INSTRUMENTATION`, precision `DERIVED`, granularity `PAGE`). The
NVIDIA backend allocates real CUDA memory, transfers to and from the device,
attempts a driver-loaded PTX kernel, synchronises, verifies host/device parity
and frees the memory; it publishes REAL measured transfer costs under
`APPLICATION_INSTRUMENTATION` with granularity `REGION`.

**Executing a CUDA transfer proves nothing about coherence.** No
cache-line-ownership, invalidation or peer-coherence claim is made anywhere in
this repository on this hardware.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and (on Windows) the Winsock
library. No third-party dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure     # or run the test binaries directly
```

Options: `COHERENCE_OBSERVATORY_BUILD_TESTS`,
`COHERENCE_OBSERVATORY_BUILD_EXAMPLES`,
`COHERENCE_OBSERVATORY_BUILD_TOOLS`,
`COHERENCE_OBSERVATORY_BUILD_BENCHMARKS`,
`COHERENCE_OBSERVATORY_WARNINGS_AS_ERRORS` (default ON),
`COHERENCE_OBSERVATORY_SANITIZE` (`` or `address`).

Warning configuration is `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus
/Zc:preprocessor` under MSVC and `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -Werror` elsewhere. The Release and Debug builds
of this release compile with **zero first-party warnings**, and the sanitizer
build is verified to be genuinely instrumented (a deliberate heap overflow is
caught).

## Tests

```
coherence_observatory_tests          unit, property, race, adversarial, persistence,
                                     explanation, snapshot, backend and hardening
coherence_observatory_multiprocess   real-process publisher death, reincarnation,
                                     coordinator restart, protocol adversarial
```

Both binaries run plainly and are allowed to finish naturally. There are no test
timeouts anywhere: a hanging test is a defect. The only intentional termination
is in the multiprocess suite, where killing a publisher and killing the
coordinator *are* the scenarios under test. Child-process orchestration waits for
a readiness file with a generous bound and reports expiry as a hard failure.

Coverage includes: identity and structural validation; ingestion and ordering;
counter reset, wrap, generation change and width bounds; generation fencing and
staleness; attribution outcomes; aggregation exactness, saturation and capacity;
analysis determinism; explanations and snapshots; frame encoding and adversarial
frames; persistence corruption at every boundary; seeded randomized property
tests; deterministic race interleavings; backends; hardening regressions; real
process death and restart.

## Examples

```
example_basic_exact_observation                 exact synthetic observation
example_remote_access_attribution               remote-access rows and locality
example_ping_pong_detection                     ping-pong pattern and cost
example_aggregate_counter_partial_attribution   coarse counter, honest outcome
example_publisher_fencing                       fencing and reincarnation
example_coordinator_restart                     restart and conservative recovery
example_false_sharing_granularity               granularity-aware classification
example_persistence_recovery                    integrity-checked durable state
```

All examples use only the public API.

## CLI

```
cohobs       --endpoint host:port | --state <file>   read-only inspection
cohobs-admin --endpoint host:port                    administrative mutation
cohobsd                                              coordinator daemon
cohobs-publisher                                     remote publisher process
```

`cohobs` commands: `summary`, `publishers`, `resources`, `regions`,
`generations`, `evidence`, `events`, `remote`, `invalidations`,
`transfers`, `locality`, `attribution <region>`, `provenance`,
`capabilities`, `loss`, `findings`, `hotspots`, `pingpong`,
`falsesharing`, `explain`, `json`.

`cohobs-admin` commands: `fence`, `retire-region`, `bump-topology`, `save`.

Inspection and mutation are deliberately separate executables: `cohobs` can
never mutate coordinator state. `cohobsd` prints its bound endpoint and can also
write it to a file, which is how supervisors and tests discover an ephemeral
port.

## CMake install and downstream use

```sh
cmake --install build --prefix /path/to/prefix
```

Installs public headers, the static library, the CMake package config, the
targets export, the version config and `README.md`/`LICENSE`.

Downstream:

```cmake
cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)
find_package(CoherenceObservatory CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SummonSoftwareLabs::CoherenceObservatory)
```

```cpp
#include <coherence/observatory.hpp>
#include <iostream>

int main() {
  sol::coherence::ObservatoryOptions options;
  options.observer_id = sol::coherence::ObserverId{"observer.consumer"};
  sol::coherence::Observatory observatory(options);

  sol::coherence::RegionRecord region;
  region.id = sol::coherence::MemoryRegionId{"region.consumer"};
  region.generation = sol::coherence::MemoryRegionGeneration{1};
  region.owner = "consumer";
  region.sharing_scope = sol::coherence::SharingScope::Private;
  if (!observatory.register_region(region).ok()) {
    return 1;
  }
  const auto snapshot = observatory.snapshot();
  std::cout << "regions " << snapshot->regions().size() << "\n"
            << "epoch " << snapshot->coordinator_epoch().value() << "\n";
  return 0;
}
```

## Benchmarks

```sh
./build/cohobs-bench
```

Measured scale points: 10, 100, 1,000 and 10,000 regions, over 20,000 ingested
observations. Operations measured: single-event ingestion, batch ingestion,
attribution, region lookup, snapshot capture (with and without analyzers),
hotspot analysis, ping-pong analysis, full findings, explanation rendering, and
state save/load. Every figure printed by the tool is measured on the host in
that run; nothing is estimated.

Two real bottlenecks were found and fixed during this release:

* **Hotspot analysis was scanning the whole aggregate store once per dimension.**
  It now performs a single dispatching pass and evaluates reason conditions from
  raw accumulated values before allocating a finding, its metrics or its cost
  decomposition. At 10,000 regions this took hotspot analysis from roughly
  16 ms to roughly 0.33 ms -- about 49x -- with byte-identical findings.
* **The journal analyzers rebuilt three strings per journal entry and re-assigned
  them on every hit.** They now use string views, a reused key buffer and
  first-insert-only assignment.

Remaining measured characteristics, stated honestly: snapshot capture copies the
aggregate store, so its cost grows linearly with the number of buckets
(approximately 3.8 ms for a 10,000-region store with about 100,000 buckets), and
the pattern analyzers are linear in the retained observation journal (16,384
entries), which dominates a full analyzer sweep at the largest scale.

## Genuine limitations

* No cache-line ownership, invalidation-counter or peer-coherence telemetry is
  available on this host, and none is claimed. Those semantics are exercised
  synthetically and classified UNSUPPORTED on hardware.
* The host has a single NUMA node, so cross-NUMA behaviour cannot be observed
  here; the remote-NUMA capability is UNSUPPORTED and the semantics are proven
  by the synthetic backend.
* Fewer than two NVIDIA devices are present, so peer-to-peer topology could not
  be established.
* No CXL device or pooled-memory fabric is present; CXL and pooled telemetry are
  UNSUPPORTED and only simulated.
* The CUDA kernel is loaded from portable PTX through the driver API and is not
  compiled by a CUDA toolchain; if the driver declines to load it, the backend
  reports that honestly rather than silently skipping the kernel.
* The default cost model's coefficients are illustrative documentation, not
  measurements from this host. Only explicitly measured costs and exact counter
  facts are presented as such.
* Timestamps are monotonic and process-local; only ordering within one publisher
  boot is meaningful, and cross-process timestamps are treated as opaque
  ordering hints.
* Snapshots received over the wire carry a bounded prefix of the aggregate
  buckets; the runtime reports how many were omitted rather than truncating
  silently.
* The observation journal and per-region sample windows are bounded, so pattern
  analysis sees a retained window; eviction is counted as evidence loss and
  reported as a capacity-pressure finding.
* The runtime is observational by construction: it cannot and will not change
  coherence state, ownership, placement or consistency semantics.

## Lock order

One state mutex per `Observatory` guards every field of the runtime state. The
coordinator's session mutex guards only the session map; it is never held while
the observatory is touched, while a socket is written, or while a frame is
dispatched. The I/O thread is the only thread that creates or destroys sessions.

No lock is ever held across socket I/O, filesystem I/O, callbacks, vendor APIs,
long-running analysis, thread joins or process waits, and no two locks are ever
nested. Persistence copies the durable subset under the lock and writes the file
after releasing it. There is no thread-per-connection and no thread-per-event.

## No telemetry

No telemetry transmission. No analytics. No hidden reporting. The runtime opens
no network connection other than the coordinator endpoints you configure, and
writes no file other than the state file and trace files you name.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.