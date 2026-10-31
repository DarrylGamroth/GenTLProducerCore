# Shared core review, 2026-10-31

## Scope, authority, and status

Comprehensive review of all `GenTLProducerCore` implementation and public
headers, packaging exports, and the SDK2/SDK3 metadata mappings and relevant
buffer publication/reset paths. This includes inherited code, not just the
extraction diff. Work remains isolated under
`/path/to/workspace/worktrees/gentl-buffer-info-20261031` on
`refactor/shared-buffer-info` in each repository. Baselines: core `c038116`,
SDK2 `fd20b6e`, SDK3 `05d33a6`; all original checkouts were clean.

Authority: EMVA GenTL 1.6, particularly §§4.1.2.2, 6.1.4, 6.1.6,
6.3.6.5–6, and buffer information definitions on pp.141–147. Local copy is
`deps/GenICam_Package_2025.10/GenTL/GenICam_GenTL_1_6.pdf` under the isolated
workspace. Vendor/EMVA headers remain unmodified.

Two independent reviewers assessed ports/info/package handling and buffer
metadata/producer mappings respectively. The primary adjudicates findings,
checks significant fixes, and verifies integration. Prior extraction tests
passing established behavior preservation, not full protocol conformance.

the host application retains the SDK2 camera. No physical camera access, settings changes, active CTI rebuild, or deployment
was performed as part of this review. Publication of the reviewed branches
was subsequently authorized; it leaves the host application's active build untouched.
SDK2 verification uses a mock SDK; SDK3 uses the vendor simulator. Physical
SDK2/SDK3 camera qualification remains a separate gate.

Review and corrective implementation are complete for the shared library and
the identified metadata/queue races. Independent corrective reviews found no
remaining blocker within that scope. The reviewed implementation is delivered on `refactor/shared-buffer-info`
branches in the core and both producer repositories. It is not deployed to
the host application or merged into the original checkouts.
Hardware qualification, vendor sanitizer reports, and source-handle destruction
are explicitly outside the completed validation claim.

## Findings and disposition

| ID | Severity / confidence | Evidence and consequence | Adjudication / remediation |
| --- | --- | --- | --- |
| CORE-P1 | P2 / confirmed | Data-backed RegisterRegion with explicit size greater than backing data permits out-of-bounds memcpy; independent ASan repro | Fixed and regression-tested: validate backing bounds, including empty data and malformed address ranges; callback-backed empty regions stay valid |
| CORE-P2 | P2 / confirmed | integer_register rejects valid partial writes; violates §6.3.6.6 alignment/RMW handling | Fixed and regression-tested: preserve unaffected bytes, reject unreadable partial updates, preserve full setter-only writes; serialize full helper RMW |
| CORE-P3 | P2 / confirmed | Failed port access leaves requested byte count although no bytes transferred | Fixed and regression-tested: actual transfer counts, explicit callback failure contract; no automatic retry of failed hardware writes |
| CORE-P4 | P2 / confirmed | Relative dlopen path resolves differently after chdir, including first query after chdir | Fixed and regression-tested: preserve module location independently of later working-directory changes; SDK-free DSO regression |
| CORE-P5 | P2 / confirmed | File URL path contains raw space, #, ?, % and non-ASCII bytes | Fixed and regression-tested: RFC3986 path encoding with round-trip regression |
| CORE-P6 | P2 / confirmed | URL hardcodes schema 1.1 while XmlFileInfo metadata can specify another version | Fixed and regression-tested: generate schema query from supplied metadata |
| CORE-P7 | P2 / confirmed | Empty default string_view supplies null source to memcpy(length=0); reproduced by UBSan | Fixed and regression-tested: skip copying an empty value while still writing terminating NUL |
| CORE-M1 | P2 / confirmed | Unknown buffer info command returns INVALID_ID instead of required NOT_IMPLEMENTED | Fixed in dispatcher/contract/tests; unsupported standard and custom queries verified |
| CORE-M2 | P2 / confirmed | IS_QUEUED excludes acquiring/output-owned buffers in both producers | Fixed mapping of producer-owned states; input, acquiring, output, retrieval/discard assertions pass |
| CORE-M3 | P2 / confirmed | SDK2 DATA_SIZE reports retained payload after input requeue | Fixed; same requeue assertion fails before and passes after, preserving captured geometry |
| CORE-M4 | P2 / confirmed | SDK3 incomplete kill DATA_SIZE incorrectly equals zero filled bytes | Fixed with captured payload_size; kill and requeue assertions pass |
| CORE-M5 | P2 / delivered height confirmed; image-present interpretation supported | SDK3 zero-byte kill reports full delivered image height and image-present=true | Fixed zero-byte killed delivery; ordinary complete-image presence remains verified |
| CORE-M6 | P1 consequence / reproduced by deterministic test in both producers | Discard can clear ownership between metadata commit and event publication; revoke can free buffer before publication dereference, or requeue can create a stale event | Fixed stream-coordinated publication/discard; deterministic regressions fail before and pass after in both producers |

An independent corrective review additionally identified **CORE-M6-R1**
(P1 consequence): EventGetData and EventFlush detached raw payloads before
taking the stream lock. A deterministic EventFlush/requeue/new-delivery test
reproduced stale ownership in both producers. The fix coordinates queue
consumption/discard under stream→event locking. Waits hold only the event
mutex; retries keep the original timeout deadline. The same tests now pass,
and a separate reviewer accepted the fix. Concurrent source-handle destruction
remains a deferred handle-lifetime boundary: consumers must keep source handles
open until active operations finish. This review does not certify arbitrary
concurrent Close/Unregister combinations or all producer GenTL entry points.

CORE-M2 and CORE-M3 were independently confirmed by the primary against the
specification and fail-before producer regressions. Before-fix logs:
`review-sdk2-before.log` (DATA_SIZE reset assertion),
`review-sdk3-before.log` (producer ownership assertion). CORE-P1 has an
independent ASan repro; ports regression runs also reproduce P2–P7. Exact
source/test changes and final results follow below when verified.

## Boundaries for a future callback-sdk producer

Assuming the requested SDK is FixtureVendor's callback-sdk, local vendor
headers are available at
`/path/to/callback-sdk_C_V2.h`.
They define per-context discovery/control, SDK ring buffers and raw-image
pointers, and image callbacks with a `beforeCopy` selection. This is header
inspection only, not SDK execution, a vendor lifetime guarantee, or hardware
validation. No callback-sdk producer is implemented by this work.

| Concern | Shared core suitability / producer obligation |
| --- | --- |
| Image format | info_buffer currently describes contiguous little-endian PFNC image payloads without chunks; normalize into that representation or keep unsupported format queries local |
| Signed samples | Header exposes isUnsignedPixel/isMono8Pixel; choose truthful PFNC encoding or an explicit tested conversion; do not label signed data Mono16 unsigned |
| Vendor memory | Keep callback/ring-buffer lifetimes vendor-specific; never retain a raw pointer past its documented validity; copy into an owned GenTL buffer unless a supported loan contract exists |
| Callback delivery | Define bounded ownership, full/empty queues, drops, stop ordering, callback drain/unregistration, and restart before sharing lifecycle code |
| Frame metadata | Capture dimensions, padding, intended size, format and frame identity at delivery; preserve old buffer metadata across camera reconfiguration |
| Time | Shared timestamp value is nanoseconds for both GenTL queries; convert vendor ticks before supplying it, and declare the corresponding frequency |
| Image tags / chunks | A vendor image tag is not automatically GenTL chunk data; advertise only supported, parsed payload structures |
| Control errors | Fallible SDK reads/writes need RegisterRegion access callbacks that return GenTL errors; convenience integer getters cannot propagate a vendor read error |
| Threading | Callback thread, SDK control thread, consumer event handling and device close need an explicit tested ownership contract; do not force SDK2's copy loop or SDK3's direct queue strategy onto it |

The core is useful for that producer as protocol formatting/port infrastructure.
Sharing the acquisition state machine is neither required nor justified by the
current evidence. Native SDK versions, frame grabber, camera model, pointer
validity, callback shutdown guarantees and image stride/tag layout need to be
qualified for the chosen callback-sdk configuration.

Additional accepted limits: Linux x86-64 is the validated producer platform;
native-endian integer helpers must not silently claim big-endian portability.
Callback exceptions and allocation/resource exhaustion at C ABI boundaries
need explicit producer policy; this review does not simulate process-wide OOM.


## Verification recorded so far

Core Release and ASan/UBSan tests pass, including the new malformed-region,
partial-write, count, URL, runtime-location and empty-string cases. Core tests
also pass under ThreadSanitizer (`review-tsan-tests.log`). Independent review
of the P1–P7 corrections found no blocking issue. The per-register mutex guards
helper calls, not arbitrary external state mutation or operations spanning
different registers. The forced-overlap regression uses a bounded scheduling
opportunity; observed fail-before/pass-after and TSan are evidence, not a proof
of all schedules.

The primary separately verified file URL parsing/decoding with Python's
`urllib.parse`, round-tripping spaces, #, ?, %, and UTF-8 filenames while
checking schema 1.0.0. Evidence: `review-uri-roundtrip.log` and
`url_roundtrip.cpp` under the isolated workspace. CMake now discovers Threads
both when building the core and when consuming the installed static target.

Custom access callbacks/mutable data regions must honor their documented
synchronization, all-or-nothing byte transfer, and non-reentrancy contracts.
The callback signature cannot describe partial successful transfer. A hardware
setter error may still require vendor-specific state recovery; the core must
not retry or claim recovered hardware state. Physical and resource-exhaustion
qualification remains separate from protocol/memory-safety regression tests.


## Final verification and remaining qualification

| Configuration | Result |
| --- | --- |
| Standalone core | 2/2 passed |
| Core ASan/UBSan, halt on UB | 2/2 passed |
| Core ThreadSanitizer | 2/2 passed |
| SDK2 source-core build | 6/6 passed (mock SDK, controls, XML, shared tests) |
| SDK2 installed-core build | 4/4 passed |
| SDK2 ThreadSanitizer native tests | 5/5 passed, including deterministic queue races |
| SDK3 source-core build | 5/5 passed, including instrumented and production CTIs under simulator |
| SDK3 installed-core build | 3/3 passed |
| SDK3 ThreadSanitizer | Functional simulator checks pass but both simulator tests fail sanitizer exit status on four vendor-library race reports each; not a clean sanitizer result |
| SDK3 unchanged baseline ThreadSanitizer | Same four reports at the same vendor symbols/offsets; unchanged producer/core source baselines, same SDK binary |
| Test-hook exports | Absent from both production CTIs; confined to test targets |
| Original SDK2 production binary | SHA-256 unchanged: 84a4db5b1f54d2575755febda48c2953db7df1cca1542a0c76df05e4e967ec46 |

The full SDK2 TSan invocation also attempted the Python/ctypes XML test; it
failed loading libtsan (`cannot allocate memory in static TLS block`), before
calling the producer. The native-only invocation passes all five applicable
tests. XML validation passes in both ordinary build configurations. No
suppressions or relaxed sanitizer exit settings were used to manufacture a
passing result.

SDK3 reports originate in unchanged `libatdevsimulator.so` (BuildId
`b7f6b9aa24ca7e59af6902dbe1dd6d9754c4447d`):
TcontinuousFilePath operator= offsets 0x3041e/0x3046f, operator+ 0x304ef, and
TcontinuousDynamicLoader::Open offset 0x32228. The identical reports on the original
SDK3/core baseline establish that they are not introduced by this extraction.
They do not establish whether the vendor code has a true race or synchronization
that the sanitizer cannot observe. No speculative SDK workaround was added.
Vendor review/instrumented SDK evidence is needed for a clean SDK3 race claim.

SDK3 links also emit `.dynsym local symbol ... >= sh_info` diagnostics from
unchanged `libatcore.so.3.12.30012.0`; the unchanged baseline emits the same
messages. Builds and ordinary runtime tests succeed. No compiler warning or
new shared-core linker diagnostic was observed.

Primary verification logs under the isolated workspace:
`review-core-{build,tests,install}.log`, `review-core-asan-{build,tests}.log`,
`review-tsan-tests.log`, `review-sdk{2,3}-{build,tests}.log`,
`review-sdk{2,3}-installed-{build,tests}.log`,
`review-sdk2-tsan-native-tests.log`, `review-sdk{2,3}-tsan-tests.log`, and
`review-sdk3-baseline-tsan-{configure,build,tests}.log`. Fail-before/pass-after
queue evidence uses `metadata-publication-sdk{2,3}-*` and
`metadata-consumer-sdk{2,3}-*`. The core reviewer's original sanitizer repros
and independent rebuilds remain under `/path/to/port-review-build` and
`/path/to/review-build`.

Relevant source locations (function names remain stable as lines change):

- `src/port_model.cpp`: contains, integer_register, PortModel::read/write,
  module-path initialization, make_file_url/make_file_urls.
- `src/info.cpp`: copy_string; `src/buffer_info.cpp`: info_buffer.
- Both producer `src/core/cti_exports.cpp`: DSGetBufferInfo, worker publication,
  DSFlushQueue, EventGetData and EventFlush; SDK3 also kill publication.
- SDK3 `gentl_state.hpp`: per-buffer intended payload size.
- Both producer conformance tests and test-only `publication_barrier.hpp`;
  core unit tests and SDK-free runtime-path MODULE fixture.

The shared core is suitable for continued SDK2/SDK3 integration under these
contracts and for a future normalized callback-sdk image path. Release qualification
still needs physical SDK2 testing after the host application releases the camera and physical
SDK3 testing for the actual model/configuration. simulator is not that hardware
qualification. Keep the core SDK-free; merge core API before adapter adoption.
Do not extract a universal acquisition state machine until all three memory
ownership strategies and shutdown/closure contracts have evidence.
