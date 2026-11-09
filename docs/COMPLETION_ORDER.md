# Queued completion order — 2026-11-03

Core 0.2.3 adds opt-in completion by buffer address for the experimental
camera adapter out-of-order backend producer. Existing queued backends remain FIFO; copy
acquisition is unchanged. The GenTL C entry points and event payloads are unchanged.

The checkout started at commit `c534bbc0b1ca7e9d22048a4924efd1b4cee4f22f`
with the preceding task's seven modified files implementing native-pool
revocation/version 0.2.2. This work extends those changes in place; none were
discarded. continuous SDK2, SDK3, triggered and PipeWire sources remain unchanged.

## Contract and mechanism

`QueuedBackend::completion_order(device)` defaults to `kFifo`. A backend
returning `kAny` promises nonblocking `wait_frame(timeout_ms=0)` and can complete
any accepted address, once per submission. The mode is captured before worker
creation and remains immutable during that session. Returned addresses must
still belong to the current outstanding set; announcement alone is insufficient.
The native backend must prevent stale completions across reuse of the same
address. out-of-order backend also validates its unique per-delivery lease token.

The single worker continues to perform every native submission and completion
poll. `kAny` searches the outstanding deque and erases only the matching entry.
The scan and middle removal cost O(N), bounded by announced/submitted buffers;
out-of-order backend additionally limits its fixed pool to 64. The default FIFO identity
check/removal retains its front-only path. Existing state-refresh scans remain.
No new image allocation or copy is introduced; this is not a whole-worker
zero-allocation claim (event containers and setup retain their existing costs).

For `kAny`, the worker submits all available input, regardless of the remaining
finite delivery count. Example: a physical A/B ring has delivered both images
with one delivery remaining; the consumer returns B then A. Submitting only B
would withhold A, which is needed to retire the contiguous A/B prefix. Submitting
both permits capture of A; the runtime publishes that final requested frame and
stops. Extra pending/complete native buffers remain borrowed until confirmed
stop and produce no extra event. FIFO keeps its existing finite submission cap.

The worker polls `kAny` completions without blocking, then uses an interruptible
100 µs idle wait if no frame is ready. With no native buffers outstanding it
waits for input or stop. Queue, flush, host stop and close signal the condition
variable while holding the predicate mutex. This avoids delaying ring returns
behind a one-second backend wait or the FIFO starvation sleep. The 100 µs value
is a polling policy, not a measured timing guarantee; OS scheduling can overshoot.
FIFO retains its 1000 ms backend wait and 5 ms input-starvation recovery sleep.

Every outstanding `kAny` buffer is conservatively potentially acquiring. The
runtime cannot infer a unique active head, so KILL does not fabricate a head
incomplete event. It returns outstanding buffers to input only after confirmed
retirement. FIFO retains its existing flags and KILL head event. Completed
metadata, frame IDs, counters and new-buffer events follow completion order.

## Ownership and publication

| State | Writers/readers and synchronization |
| --- | --- |
| Input queue and buffer flags | Host queue/flush and worker under stream mutex. A buffer cannot be submitted twice or revoked while queued. |
| Outstanding deque | Worker admission/completion and retired cleanup under stream mutex. Matching happens before mutation or event publication. |
| Native payload | Backend owns it until successful completion; SDK/DMA coherence is a backend responsibility. Consumer queue transfers it back through worker submission. |
| Completion metadata/events | Worker commits metadata and publishes under stream mutex, then event mutex; event consumption uses the same ordering. |
| Idle predicate | Worker, queue/flush and stop/close share stream mutex; CV wait atomically releases it and rechecks the predicate, preventing lost wakes. |
| Stop/reclamation | Existing lifecycle/retirement locks serialize host joins/stops. Failed retirement retains unmatched native ownership. Native-pool revoke veto still applies after delivery. |

No lock-free protocol, volatile publication or new native-callback synchronization
is introduced. Stream mutex release/acquire publishes host state; bridge mutex
and SDK operations retain their separate responsibilities. C++ exceptions and
allocator behavior are unchanged. Hook installation, setup and error formatting
are outside any latency claim.

## Evidence and independent review

| ID | Evidence / confidence | Disposition and checks |
| --- | --- | --- |
| ORDER-1 | Observed: old worker rejected valid non-head completion. New test compiled with the new API but unchanged worker failed awaiting the B event. | Address matching fixes it; same regression now passes with B/C/A and re-submitted B, correct event identity and metadata. |
| ORDER-2 | Observed: adapter rejected B return before A. Revised direct regression failed at that return before the adapter change. | Removed return cursor guard, retained physical ring checks and bridge tokens. Same regression passes; held A remains unchanged and credits stay withheld until A returns. |
| ORDER-3 | Derived: finite submission cap can withhold the address needed for hardware retirement. | `kAny` submits all returns. Generic and actual core/adapter/bridge tests verify reverse returns with fewer deliveries remaining than pool size, exact count and no extra event. |
| ORDER-4 | Observed in software: long backend wait delayed ring returns in the integration test. | Nonblocking polls plus queue/stop/close wakeups. Tests check timeout 0 versus FIFO 1000 and queue/stop/close outcomes with no pending native buffers. Idle-entry timing is not forced by a test hook; lost-wakeup reasoning was reviewed independently. |

`tests/queued_completion.cpp` also checks null/foreign/duplicate/unsubmitted
identities, unchanged default FIFO rejection, finite count shorter than pool,
failed retirement after non-head completion, retained membership and revoke/
flush guards. Existing retirement tests retain FIFO stop/failure/KILL coverage.

camera adapter's test uses the actual queued runtime, adapter and bridge with fake
DCAM/native operations: A/B events, return B while holding A, no credits, return
A, credits for both, physical A/B completion against B/A submission order,
continuous/finite runs, stop/flush/restart and revoke. It calls no image-copy API.

Independent Sol review inspected ordering, finite submission, ownership, flags,
KILL and the subsequent CV notification paths. No confirmed software blocker
remained. Software evidence does not establish physical DMA, hardware credits
or callback quiescence on the camera.

## Compatibility and reproduction

Build environment: Linux x86-64, C++20, GCC/libstdc++ 14.2.0, glibc 2.41,
system allocator and pthread-backed standard mutex/CV/thread. Warning flags
include `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`; tests enable assertions.
Normal camera adapter build has no build-type override; sanitizer build is Debug.

| Producer | Contract preserved | Verification against this core source |
| --- | --- | --- |
| camera adapter copy + experimental | Copy unaffected; out-of-order backend explicitly opts into `kAny`. | Both CTIs build; 14/14 CTest checks including all eight core checks. |
| continuous SDK2 | CopyBackend; no source changes. | `build-deb-validation`: 11/11 tests including mock conformance, stop priority and camera controls. |
| triggered pdv | CopyBackend; no source changes. | `build-genapi351`: 11/11 tests including image layout/dynamic geometry smoke. |
| continuous SDK3 | Default FIFO plus its own SDK FIFO validation; no source changes. | `build/core-any-order`: 10/10 tests including native stop fault and XML. Optional GenApi-C/simulator tests unavailable. Vendor libatcore emits malformed `.dynsym` local-symbol linker warnings; links succeed. |

Wrapper source trees were clean before and after the builds. Existing stale
dependency cache paths required reconfiguration to available SDK/header paths;
these were build-cache changes only. Rebuild queued CTIs with matching headers
and static archives; mixing old objects with the new virtual interface is not
supported.

From `camera adapterGenTL`, normal checks use `cmake --build build -j4` and
`ctest --test-dir build --output-on-failure`. Focused sanitizer checks use
the existing Debug `build-sanitize` with
`-fsanitize=address,undefined -fno-omit-frame-pointer`, leak detection enabled,
and the queued completion/retirement plus out-of-order backend patch/backend tests.
All four focused ASan/UBSan tests passed with leak detection. A separate Debug
`build/tsan` using `-fsanitize=thread -fno-omit-frame-pointer` passed both the
queued completion and full out-of-order backend runtime/adapter tests with no race report.

No physical camera or latency benchmark ran. The fixed hardware ring still
waits for an earlier held buffer even when later buffers have returned; this
change permits those returns without granting premature hardware reuse.

## Completion notifications, core 0.2.4

The user requested removing the 100 µs timer for out-of-order backend while preserving other
wrappers. `QueuedBackend::set_completion_callback` defaults to false and is only
queried for `kAny`. False preserves the existing timed fallback. out-of-order backend returns
true and forwards the callback to its existing native interception bridge.
The bridge signals after `internal_release` publishes the completed entry, and
on poison and begin/end stop. DCAM's configured native callback is preserved.
An earlier hardware interrupt alone would not end DCAM's lease.

| State | Publication/lifetime rule |
| --- | --- |
| Bridge completion and fault | Published under bridge mutex before notification; consumer rechecks under that mutex. |
| Shared wake generation | Native callback and host queue/flush/stop/close increment with release and notify; worker snapshots/waits with acquire. |
| Worker snapshot | Taken before input admission and completion/status check. An event before the snapshot is visible to the check; an event afterward changes the value, so atomic wait cannot sleep on an already-consumed notification. |
| Readiness | Persists in the completed queue, input queue, fault state or stop flag; one wake can represent multiple events. Spurious wakes only cause another check. |
| Callback context | Owns shared wake state, contains no stream pointer and takes no stream/backend mutex. It must not block, throw or re-enter. |
| Clear | Setter serializes with invocation. Start failure clears immediately; stop/close clears after joined retirement. A failed retirement retains shared state until retry. |

No stream mutex is held during atomic wait. The bridge callback therefore cannot
invert the existing stream → backend → bridge lock order. The worker retains a
local shared reference until exit; a retained callback cannot dereference a
destroyed stream. No callback may execute after CTI unload; native quiescence
and the out-of-order backend bridge's existing DSO pinning remain required.

The review identified a fault case when the consumer held every image: waking
then immediately sleeping for new input would ignore a native fault. The
notification-enabled worker now performs nonblocking status checks even when
its outstanding deque is empty. out-of-order backend checks health without attaching when
the initial pool is still empty. Unsupported-callback and FIFO paths retain
their former empty-input behavior.

Common-path additions are one callback indirection plus an atomic increment and
notify per completed internal release, and a readiness check after wake. Callback
storage/shared state are created at setup; no image copy or helper thread was
added. There is no periodic timer on the opted-in path. `atomic::wait` may perform
its standard-library spin/futex work; no latency or CPU-budget bound is claimed.

Software evidence:

- The polling fallback continues making readiness calls while idle; the
  notification-enabled call count remains stable over the test's idle window.
  This verifies scheduling behavior, not camera latency.
- A completion published by the fake backend immediately after its readiness
  check but before core parking still delivers, proving the missed-wake edge.
- One wake for a burst delivers every queued completion; spurious notifications
  are harmless. Inline startup faults and empty-outstanding held-pool faults
  cause retirement without another consumer return.
- Start failure removes the callback. Stop failure/retry preserves it until
  retirement; an in-flight shared callback copy can outlive stream cleanup
  without referring to stream storage.
- Actual core/adapter/bridge integration uses the notification path for reversed
  returns and includes native completions inline during SDK start. Bridge tests
  distinguish get from internal release, notifications from hardware credits,
  fault/stop notification, callback clear and early startup completion.
- Independent Sol review found no remaining confirmed concurrency blocker after
  the empty-outstanding fault fix. Callback contract and remaining hardware
  boundaries were reviewed separately from software checks.

The other three wrappers rebuilt against core 0.2.4 without source changes:
SDK2 11/11, SDK3 10/10 and triggered 11/11 tests passed. SDK3's vendor linker warnings
and unavailable optional GenApi-C test remain as documented above. Normal and
sanitizer reproduction use the same commands/build directories above.
Final notification implementation: all 14 normal checks, four focused
ASan/UBSan checks with leak detection, and both ThreadSanitizer checks passed.
No camera callback-latency measurement or physical acquisition was performed.
