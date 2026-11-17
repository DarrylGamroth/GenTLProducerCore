# GenTLProducerCore publication review

Reviewed the original main branch at `cb59b23`, including the base helpers,
copy and queued runtimes, tests, CMake exports, build inputs, and reachable
Git history. The public license is MIT. The EMVA GenTL header is downloaded
and verified during configuration. The reviewed repository history contains
no vendor SDK files or binary payloads. It does retain workstation paths,
downstream repository names, and commit references in development records.
The existing v0.2.5 release has no attached binaries.

| ID | Severity | Finding | Validation |
| --- | --- | --- | --- |
| CORE-01 | High | Copy acquisition could throw across the GenTL ABI if worker creation failed after starting its backend. | The test runtime injects thread creation failure and checks rollback, failed rollback, and a successful stop retry. |
| CORE-02 | Medium | GenApi stream registers and GenTL disagree on native buffer minima and the number of queued FIFO buffers. The minimum mismatch was reproduced on the triggered mock. | Runtime tests compare the stream port registers with `DSGetInfo` for both cases. |
| CORE-03 | Medium | Copy buffer metadata lost the backend's image padding. | Runtime tests check padding across configuration changes and reuse of a delivered buffer. |
| CORE-04 | Medium | Copy acquisition faults were discarded when backend stop succeeded. | The copy mock injects a worker error and checks the stop status and diagnostic. |
| CORE-05 | Medium | Both runtimes discarded a feature invalidation when the destination was too small and retained a New Buffer event when the destination was null. GenTL 1.6 §4.2 and §6.3.7.2 require retry on error and discard on a null destination. | Both mock runtimes preserve the feature event for retry and allow the discarded New Buffer to be queued again. Iris uses the specified retry and discard behavior. |

All ten Core test suites pass with GCC coverage enabled. The suite covers
94.0% of production lines (3,833 of 4,077) and 61.4% of branches using
the same source filters as CI. The ABI tests exercise both ownership models,
including module discovery, port metadata, stacked access, events, and
buffer ownership. Existing mocks cover native buffer ownership and failed
retirement. The general exception boundary and concurrent handle destruction
limits recorded in [the prior review](REVIEW.md) still apply. Hardware and
timing qualification is outside this review.

[EMVA GenTL 1.6 specification](https://www.emva.org/wp-content/uploads/GenICam_GenTL_1_6.pdf)
