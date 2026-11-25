FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc \
    && echo 'deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
       build-essential binutils clang-18 clang-tools-18 llvm-18-dev libclang-18-dev lld-18 libgc-dev libuv1-dev libsqlite3-dev libjansson-dev python3-pip \
    && python3 -m pip install --no-cache-dir --no-deps --only-binary=:all: cmake==3.30.9 ninja==1.11.1.4 \
    && clang-scan-deps-18 --version
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY compiler/ compiler/
COPY cmake/ cmake/
COPY include/ include/
COPY runtime/ runtime/
COPY std/ std/
COPY tests/ tests/
COPY examples/compiler-smoke/ examples/compiler-smoke/
COPY examples/http-sqlite/ examples/http-sqlite/
COPY examples/jsonlines/ examples/jsonlines/
RUN sh tests/jammy-smoke.sh build /out/apps \
    && cmake --install build/release --prefix /out/sdk

FROM ubuntu:22.04 AS bare-sdk
COPY --from=builder /out/sdk/ /sdk/
COPY --from=builder /src/examples/jsonlines/ /work/jsonlines/
COPY --from=builder /src/examples/http-sqlite/ /work/http-sqlite/
COPY --from=builder /out/apps/http-fixture.db /work/http-fixture.db
COPY tests/jammy-smoke.sh /verify.sh
RUN mv /sdk '/搬迁 SDK' && sh /verify.sh verify-sdk /work '/搬迁 SDK' && touch /sdk-verified

FROM ubuntu:22.04 AS verify
COPY --from=bare-sdk /sdk-verified /sdk-verified
COPY --from=builder /out/apps/ /apps/
COPY tests/jammy-smoke.sh /verify.sh
RUN sh /verify.sh verify /apps
