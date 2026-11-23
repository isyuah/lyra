FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc \
    && echo 'deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
       build-essential binutils clang-18 clang-tools-18 llvm-18-dev libgc-dev libuv1-dev libsqlite3-dev python3-pip \
    && python3 -m pip install --no-cache-dir --no-deps --only-binary=:all: cmake==3.30.9 ninja==1.11.1.4 \
    && clang-scan-deps-18 --version
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY compiler/ compiler/
COPY cmake/ cmake/
COPY runtime/ runtime/
COPY std/ std/
COPY tests/ tests/
COPY examples/compiler-smoke/ examples/compiler-smoke/
COPY examples/http-sqlite/ examples/http-sqlite/
RUN sh tests/jammy-smoke.sh build /out

FROM ubuntu:22.04 AS bare-runtime
COPY --from=builder /out/ /apps/
COPY tests/jammy-smoke.sh /verify.sh
RUN sh /verify.sh verify /apps

FROM bare-runtime AS verify
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 \
    && sh /verify.sh verify-sqlite /apps
