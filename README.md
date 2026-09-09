# GenTLProducerCore

`GenTLProducerCore` is a vendor-SDK-free C++20 static library for GenTL
producer implementations. It centralizes the phase-1 code that all current
producers use while keeping each producer repository buildable with only its
own vendor SDK.

The library contains no camera discovery, device control, buffer ownership, or
vendor headers. It depends only on the GenICam `GenTL.h` header, `dl`, and the
C++ standard library. The static library is PIC so a producer can link it into
its `.cti` and deliver one self-contained transport-layer binary.

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

## Phase-1 API

- `PortModel`, `PortDescriptor`, `PortUrlInfo`, and `XmlFileInfo`
- register constructors: `integer_register` and `string_register`
- embedded and on-disk XML URL helpers
- CTI runtime path helpers using `dladdr`, with a caller fallback path
- GenTL information sizing and copy helpers

The full migration boundary and follow-on lifecycle extraction are in
[docs/MIGRATION.md](docs/MIGRATION.md).

## Debian and Ubuntu package

The containerized package build and artifact-export commands are documented in
[`packaging/debian/README.md`](packaging/debian/README.md). The resulting
`libgentl-producer-core-dev` package contains only development files; it does
not install a runtime shared library.
