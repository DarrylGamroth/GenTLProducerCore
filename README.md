# GenTLProducerCore

[![CI](https://github.com/DarrylGamroth/GenTLProducerCore/actions/workflows/ci.yml/badge.svg)](https://github.com/DarrylGamroth/GenTLProducerCore/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/DarrylGamroth/GenTLProducerCore/graph/badge.svg)](https://codecov.io/gh/DarrylGamroth/GenTLProducerCore)

GenTLProducerCore is a C++20 static library for Linux GenTL producers. It
provides register, XML, and metadata helpers and two GenTL runtimes. Producers
supply their own backend for discovery, camera control, and acquisition.

## Build and test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

CMake downloads the standalone EMVA GenTL 1.6 headers and verifies their
SHA-256 hash. For an offline build, set `GenTLHeaders_ROOT` to the header
directory and `GenTLProducerCore_FETCH_GENTL_HEADERS=OFF`. The tests use mock
backends and require no vendor SDK or camera. CI instruments the library with
`GENTL_CORE_COVERAGE=ON` in an out-of-tree GCC build, requires above 90%
production line coverage, and reports line and branch coverage through Codecov.

## Use in a producer

```cmake
find_package(GenTLProducerCore CONFIG REQUIRED)
target_link_libraries(my_producer PRIVATE GenTLProducerCore::queued_runtime)
```

| Target | What it provides |
| --- | --- |
| `gentl_producer_core` | Register and port I/O, XML URLs, buffer metadata, and error helpers |
| `copy_runtime` | GenTL entry points for a `CopyBackend` that supplies a frame to copy |
| `queued_runtime` | GenTL entry points for a `QueuedBackend` that completes submitted buffers |

Link one runtime per CTI and provide its `ProducerDefinition`. See the
[runtime documentation](docs/RUNTIME_EXTRACTION.md) for backend ownership and
lifecycle contracts and the [Debian packaging guide](packaging/debian/README.md)
for development packages.

Project code is [MIT licensed](LICENSE). EMVA supplies `GenTL.h` under its own
terms. See [dependency and redistribution notes](docs/EMVA_DEPENDENCIES.md).
