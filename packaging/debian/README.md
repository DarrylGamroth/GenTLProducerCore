# Debian and Ubuntu packages

The multi-stage Dockerfile builds and tests the library, then exports a single
development-package artifact. It supports any compatible Debian or Ubuntu base
image with CMake 3.20 or newer.

Debian 12:

```sh
docker buildx build --platform linux/amd64 \
  --file packaging/debian/Dockerfile \
  --target artifact \
  --output type=local,dest=dist/debian-12 \
  .
```

Ubuntu 24.04:

```sh
docker buildx build --platform linux/amd64 \
  --build-arg BASE_IMAGE=ubuntu:24.04 \
  --file packaging/debian/Dockerfile \
  --target artifact \
  --output type=local,dest=dist/ubuntu-24.04 \
  .
```

The resulting `libgentl-producer-core-dev` package contains the static library,
public headers, CMake package configuration, and dependency documentation. The
official GenTL header remains an independently fetched build dependency and is
not stored in this repository or Debian package.
