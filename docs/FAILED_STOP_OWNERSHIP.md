# RUNTIME-R5: failed native stop and buffer ownership

## Decision

A queued backend reports operational success independently of memory
retirement: `StopResult { success, buffers_released }`. A stop/flush error must
not permit a producer to publish, requeue, revoke or destroy memory still in
native use. The pull/copy interface retains its bool stop result; its frame
acquisition path is unchanged.

SDK3's manual §§5.1.32–34 defines FIFO AT_QueueBuffer/AT_WaitBuffer and requires
application memory to remain valid until returned by wait or flushed. An
unsuccessful flush is not affirmative proof that the borrowed pointers were
returned. The implementation therefore retains ownership conservatively.

| Outcome | Runtime behavior |
| --- | --- |
| Operational success and buffers released | Finish stop and allow restart/cleanup |
| Operational error and buffers retained | Return error; retain memory, queue flags and handles; block unsafe mutations and allow explicit retry |
| Operational error but buffers released | Memory may leave the native queue, but preserve the operational failure and retryable acquisition state |
| Claimed success without buffer release | Do not reclaim memory; explicit retirement is still required |

Worker queue/wait/FIFO errors, finite completion and stopped notifications all
confirm retirement before returning queued memory. Failed worker cleanup does
not silently retry within the same host operation. A new explicit stop/close
request may retry. Close cascades preserve affected handles on error, and
GCCloseLib cannot destroy the backend while a TL remains open.

A successful ordinary stop remains graceful: finish/publish the current frame
before retirement. KILL may interrupt it, and cannot publish an incomplete
native-owned buffer until that memory is actually retired. This distinction is
part of the working baseline and the GenTL stop contract, not an optimization
opportunity in this fix.

## SDK3 adapter

The backend directly performs AcquisitionStop and AT_Flush. A failed flush
preserves its native FIFO and retirement-pending state. It rejects another
prepare/start/queue while recovery is pending. A failed native close preserves
the camera entry. A NOTWRITABLE stop result is accepted as already stopped only
when CameraAcquiring can positively confirm that state. Failed queries do not
invent a successful stop. The existing bool remote-control API delegates to
the structured stop operation; no frame-path wrapper is added.

## Frame-path constraints

- No added image copies, image allocation, conversion, staging queue, thread,
  per-frame mutex acquisition, or atomic operation.
- SDK3 wait_frame is source-identical to the working main baseline.
- SDK3 queue_buffer adds a pending-retirement guard inside its existing SDK
  mutex; the same caller address still goes to AT_QueueBuffer.
- The core's existing stream mutex covers native queue submission, preventing
  submission after retirement. This extends a critical section across the
  native call; it does not add a lock acquisition. Metadata/event callers may
  contend for that existing lock while submission executes.
- Lifecycle and retirement serialization are on start/stop/error/close paths.
  No hardware latency or zero-overhead claim follows from source inspection.

## Regression evidence

The SDK-free fault backend deliberately retains borrowed addresses after a
failed stop and touches that memory after attempted close cascades under
ASan/UBSan. It checks default/kill, queue/wait/FIFO/stopped/finite worker exits,
all unsafe mutation/revocation/closure guards, one retirement attempt, explicit
retry, restart and operational-error/retired-memory distinction. A camera-started
guard catches a recovery mismatch between runtime and native stop state.

SDK3's native failure test wraps the native calls executed by SDK3.cpp with a
deterministic native queue. It covers stop-command, flush and combined failures,
NOTWRITABLE status verification, FIFO retention, prepare/queue/start guards,
close failure and retry, and unresolved retirement during destruction. It links
the vendor library for unused symbol resolution without discovering a camera.

Fail-before evidence: `/path/to/before-fix.log` shows retained-session failure
with the former behavior; the native test fails the former wrapper because
prepare was allowed after a failed flush. The early R5 draft also failed the
existing SDK3 timed in-flight DEFAULT-stop test; that compatibility regression
was resolved without weakening the existing test. The final evidence below
records the completed checks.

## Final verification and review

Final results on 2026-10-31, against the same final source:

| Configuration | Result |
| --- | --- |
| SDK-free core | 5/5 passed |
| SDK2 with source / installed core | 9/9 and 4/4 passed |
| SDK3 with source / installed core | 9/9 and 4/4 passed |
| Core ASan/UBSan, halt on UB | 5/5 passed |
| SDK2/shared runtime TSan | 8/8 passed |
| Both CTIs loaded with RTLD_GLOBAL | Both load orders passed |

Logs are in the parent worktree directory with prefix `r5-final-`. Existing
SDK3 simulator conformance and production tests retain their assertions, including
in-flight DEFAULT completion and publication/discard races. The deterministic
fault test also verifies DEFAULT/KILL first-call retirement counts and the
claimed-success/buffers-retained combination. SDK3 wait_frame was compared
against baseline `1e958c3` and is source-identical.

Independent source and test review found no remaining blocking R5 finding.
The review identified an operational-error/retired-memory recovery gap and a
DEFAULT-stop regression in the draft; both were corrected and reverified.
Publication of a completed stop result under the stream mutex prevents a
transient false failure guard while the existing retirement mutex serializes
native stop attempts. No acquisition assertions were weakened to pass.

These are software and vendor simulator results. No physical camera was opened
for this work and no hardware latency measurement was made. Existing vendor
SDK3 simulator TSan reports remain outside the clean SDK2 TSan result.

## Status

RUNTIME-R5 implementation and software verification complete. The reviewed
changes are prepared for publication on `refactor/generic-runtime`. No deployment
was performed.
