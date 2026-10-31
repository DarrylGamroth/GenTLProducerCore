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
  contiguous little-endian PFNC images without chunks

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
