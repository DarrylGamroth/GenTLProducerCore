# GenTLProducerCore

`GenTLProducerCore` is a vendor-SDK-free C++20 static library for GenTL
producer implementations. It centralizes vendor-neutral port and information
code while keeping each producer repository buildable with only its own
vendor SDK.

The base library provides register, XML, metadata and error helpers. Optional
runtime targets implement the GenTL ABI, handles, events and buffer bookkeeping
for either pull/copy or directly queued buffers. Discovery and device controls
are backend operations; no target includes a vendor SDK or links a vendor
library. Dependencies are GenTL.h, Threads, dl and the C++ standard library.
Static PIC linkage keeps each CTI self-contained.

For a complete producer, link `GenTLProducerCore::copy_runtime` or
`GenTLProducerCore::queued_runtime` and supply a `ProducerDefinition` in its
assembly source. The matching backend derives from `CopyBackend` or
`QueuedBackend`. Instrumented `_test` runtime targets are development-only
archives for deterministic producer regressions; production CTIs use the
un-instrumented targets. See [the runtime extraction record](docs/RUNTIME_EXTRACTION.md)
for contracts, migration, validation and current lifetime boundaries.

## Build and install

By default, configuration downloads EMVA's standalone GenTL 1.6 header archive
and verifies its SHA-256 hash. No full GenICam package is needed.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

For offline builds, point `GenTLHeaders_ROOT` at a directory containing either
`GenTL.h` or `GenTL/GenTL.h` and disable downloading:

```sh
cmake -S . -B build \
  -DGenTLHeaders_ROOT=/path/to/headers \
  -DGenTLProducerCore_FETCH_GENTL_HEADERS=OFF
```

Consumers use the exported target after locating this package:

```cmake
find_package(GenTLProducerCore CONFIG REQUIRED)
target_link_libraries(my_producer PRIVATE GenTLProducerCore::gentl_producer_core)
```

The installed package uses the same download-or-explicit-root behavior. The
download is cached under the consumer's build directory. Set
`GenTLProducerCore_FETCH_GENTL_HEADERS=OFF` for a network-free configure.

`GenTL.h` is supplied under the EMVA GenICam license. Keep it unmodified and
follow the source/binary redistribution notice requirements in the current
[GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf).
The complete dependency and distribution summary is in
[`docs/EMVA_DEPENDENCIES.md`](docs/EMVA_DEPENDENCIES.md).

## Shared APIs

- `PortModel`, `PortDescriptor`, `PortUrlInfo`, and `XmlFileInfo`
- register constructors: `integer_register` and `string_register`
- embedded and on-disk XML URL helpers
- CTI runtime path helpers using `dladdr`, with a caller fallback path
- GenTL information sizing and copy helpers
- `BufferInfo` / `info_buffer`: common `DSGetBufferInfo` response formatting for
  contiguous PFNC images without chunks, with producer-described pixel byte order

The full migration boundary and follow-on lifecycle extraction are in
[docs/MIGRATION.md](docs/MIGRATION.md). The buffer information API boundary,
producer mappings, and verification are recorded in
[docs/BUFFER_INFO_EXTRACTION.md](docs/BUFFER_INFO_EXTRACTION.md). The full
corrective review, regressions, and hardware/vendor validation limits are in
[docs/REVIEW.md](docs/REVIEW.md).

## Debian and Ubuntu package

The containerized package build and artifact-export commands are documented in
[`packaging/debian/README.md`](packaging/debian/README.md). The resulting
`libgentl-producer-core-dev` package contains only development files; it does
not install a runtime shared library.

## Configurable stream descriptions, core 0.2.1

Backends may opt into idle remote-write stream-description refresh. The runtime
serializes those configuration transactions with stream lifecycle calls and
preserves published buffer metadata across subsequent geometry changes.
Pixel endianness and delivered image height are explicit metadata, and copy
backends may report a frame-sequence increment independently of underruns.
Defaults preserve the continuous producers' fixed-description behavior. See the
0.2.1 section of [the runtime record](docs/RUNTIME_EXTRACTION.md).

## Native buffer revocation, core 0.2.2

Queued backends may override `revoke_buffer(device, buffer, error_text)` when
completed buffers remain registered with a native pool. `DSRevokeBuffer` calls
this hook before releasing the handle or storage; rejection returns
`GC_ERR_BUSY` and preserves both. The default permits revocation. Calls are
serialized with stream lifecycle and queue submission. A stop that reports
`buffers_released` must retire every native mapping, because stream close can
then destroy all announced storage without individual revocation calls.

## Completion order, core 0.2.3

Queued backends default to `BufferCompletionOrder::kFifo`: `wait_frame` must
return the oldest submitted address, preserving existing SDK3 behavior.
Override `completion_order(device)` to return `kAny` when a backend can complete
any currently submitted address. The runtime snapshots the mode at acquisition
start, validates the returned address against outstanding submissions, and
publishes events/metadata in backend completion order. Each submission permits
one completion. Null, unknown, duplicate and announced-but-unsubmitted addresses
stop acquisition and require normal backend retirement.

In `kAny` mode all available input buffers are submitted, including when fewer
finite deliveries remain than there are buffers. A native fixed ring may need
every consumer return to release its retirement frontier. Only the requested
number of frames is published; remaining submissions stay borrowed until an
explicit `buffers_released` stop result. All outstanding addresses are treated
as potentially acquiring. KILL returns them to input after retirement without
inventing a single active-head incomplete event. Default FIFO behavior is
unchanged, including its finite submission cap and KILL head event.

The `kAny` worker requires nonblocking `wait_frame(timeout_ms=0)` polls. Without
completion notifications it retains the interruptible 100 µs polling fallback.
This avoids withholding returned ring buffers behind a long backend wait.

## Completion notifications, core 0.2.4

An optional `set_completion_callback(device, callback)` lets a `kAny` backend
replace timed polling with notifications. The default returns false. Opted-in
backends must notify after publishing a completion or entering a fault/stopped
state, and allow nonblocking status checks even when no addresses are currently
submitted. Empty callbacks unregister; registration/replacement synchronize
with callback invocation. Callbacks must not block, throw or re-enter the backend.

The worker snapshots a shared generation counter before checking input and
completion readiness, then uses atomic wait if no work is ready. Completion,
consumer queue/flush, stop and close all advance that counter. Notifications
before parking cannot be lost; bursts can coalesce because readiness persists.
Callbacks capture only shared wake state, never the stream. They are cleared
after joined retirement (or failed start), preserving lifetime on failed cleanup.
No periodic timer or helper thread is used for this path. FIFO, copy and
unsupported-callback fallback behavior remain unchanged. Hardware scheduling and
backend work still determine latency; this is not a measured latency bound.

Copy backends are unaffected. No producer has to opt in except one requiring
this completion contract. Rebuild queued CTIs with matching core headers and
static archives. See [the ordering review](docs/COMPLETION_ORDER.md) for ownership,
compatibility checks and remaining hardware/timing limits.


## Native buffer pools, core 0.2.5

A queued backend can expose a fixed producer-owned pool through
`DSAllocAndAnnounceBuffer`. The stream reports the required announcement
count and rejects consumer allocations. Mapped addresses remain valid across
stop and restart. After retiring DMA, the backend releases the mappings on
stream close. Consumers return buffers individually with `DSQueueBuffer`.
