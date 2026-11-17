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

All eight Core test suites pass with GCC coverage enabled. The current
suite covers approximately 61% of production lines and 39% of branches.
These results cover software mocks, including native buffer ownership and
failed retirement. The general exception boundary and concurrent handle
destruction limits recorded in [the prior review](REVIEW.md) still apply.
Hardware and timing qualification is outside this review.
