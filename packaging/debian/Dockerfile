# syntax=docker/dockerfile:1.7

ARG BASE_IMAGE=debian:12
FROM ${BASE_IMAGE} AS build

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install --yes --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      dpkg-dev \
 && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src

RUN multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)" \
 && cmake -S . -B build-deb \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_INSTALL_LIBDIR="lib/${multiarch}" \
 && cmake --build build-deb --parallel \
 && ctest --test-dir build-deb --output-on-failure \
 && cmake --build build-deb --target package \
 && ! dpkg-deb --contents build-deb/*.deb | grep -E '/GenTL.h$' \
 && mkdir /out \
 && cp build-deb/*.deb /out/

FROM scratch AS artifact
COPY --from=build /out/ /
