# Final runtime review, 2026-10-31

## Scope and result

No confirmed blocking findings. This was a read-only production-code review
of the pending `refactor/generic-runtime` changes in all three isolated
worktrees. Baselines: core `8469445`, SDK2 `795383c`, SDK3 `1e958c3`.
The existing uncommitted extraction and RUNTIME-R5 changes were the review
input. No production code was changed, committed or pushed during this pass.

A separate Sol reviewer assessed queued ownership, worker exits, stop/retry,
DEFAULT/KILL, flush/revoke, close cascades, native SDK3 retirement and remote
commands. The primary reviewed producer assembly, dependency/ABI boundaries,
alignment/allocation and the moved SDK2 worker against its baseline. Mechanical
code-quality signals did not establish a defect: ownership-specific state and
delivery paths remain separate deliberately, while common ABI implementation
is shared.

## Ownership and frame path

| Resource | Ownership and publication |
| --- | --- |
| Queued buffers and stream flags | Worker and host operations use the existing stream mutex; submission is covered by that lock |
| SDK3 image storage | The caller address goes directly to AT_QueueBuffer; successful FIFO wait or affirmative flush retirement ends native use |
| Completed frames | Stream state and event publication share stream-to-event lock ordering; consumers observe publication under those locks |
| Native stop result | Retirement mutex serializes attempts; completed result and error publish under the stream mutex |
| Host start/stop/close | Lifecycle mutex serializes stream joins; failed retirement preserves handles and storage for retry |
| SDK2 image delivery | Existing pull/copy worker flow is preserved; no SDK3-style native loan is introduced |

SDK3's native wait implementation is unchanged from baseline. No extra image
copy, image allocation, staging queue, worker thread or per-frame lock
acquisition was introduced. The existing stream lock now spans queue submission;
this can increase contention duration and is not a measured zero-overhead claim.

The reviewed implementation is C++20 on Linux x86-64, GCC/libstdc++ 14.2.0,
using standard mutexes, condition variables, threads, RAII and the vendor C ABI.
Source producer tests use Debug builds; sanitizer configurations use ASan/UBSan
or TSan with frame pointers. Native SDK3 waits occur outside the feature mutex.
Storage is allocated at announce time and remains valid through native use.
Input capacity follows announced buffers, and finite acquisition limits queue
submission. This review makes no new allocation-budget, timing-distribution or
arbitrary concurrent-destruction claim.

## Candidate disposition and evidence

| Candidate | Disposition |
| --- | --- |
| FINAL-R1: remote stop can be followed by another worker cleanup attempt | Coverage/documentation boundary; not a confirmed safety or compatibility defect. Remote AcquisitionStop calls the native wrapper directly, but pending/armed guards reject prepare/start/queue after failed retirement, and runtime cleanup still requires affirmative buffer release |

The independent reviewer reran `gentl_queued_retirement_tests` and
`continuous3-native-stop-failure`; both passed. The completed final matrix in
[FAILED_STOP_OWNERSHIP.md](FAILED_STOP_OWNERSHIP.md) remains the broader evidence:
core 5/5, SDK2 and SDK3 source builds 9/9 each, installed builds 4/4 each,
core ASan/UBSan 5/5, SDK2/shared-runtime TSan 8/8, and both CTI load orders.
Existing acquisition assertions were preserved.

## Remaining limits

Native wrapper and runtime fault injection are separate tests. There is no
end-to-end fault-injected GCWritePort(AcquisitionStop) test; this does not expand
the supported failure claim. Physical-camera SDK failures and hardware latency
were not measured. Arbitrary concurrent destruction of source handles remains
separate lifetime work. Existing SDK3 vendor simulator TSan reports are not covered
by the clean SDK2 TSan result.
