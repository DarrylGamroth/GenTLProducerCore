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

`GenTLHeaders_ROOT` is deliberately explicit. Set it to the unpacked GenICam
package root, whose `GenTL/GenTL.h` file is used at configure time.

```sh
cmake -S . -B build -DGenTLHeaders_ROOT=../GenICam_Package_2025.10
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

Consumers use the exported target after locating this package:

```cmake
set(GenTLHeaders_ROOT /path/to/GenICam_Package_2025.10)
find_package(GenTLProducerCore CONFIG REQUIRED)
target_link_libraries(my_producer PRIVATE GenTLProducerCore::gentl_producer_core)
```

The installed package requires the same explicit `GenTLHeaders_ROOT` setting,
because `GenTL.h` is an SDK header supplied by the consumer rather than copied
into this repository or its install tree.

## Phase-1 API

- `PortModel`, `PortDescriptor`, `PortUrlInfo`, and `XmlFileInfo`
- register constructors: `integer_register` and `string_register`
- embedded and on-disk XML URL helpers
- CTI runtime path helpers using `dladdr`, with a caller fallback path
- GenTL information sizing and copy helpers

The full migration boundary and follow-on lifecycle extraction are in
[docs/MIGRATION.md](docs/MIGRATION.md).
