FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc \
    && echo 'deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends build-essential cmake clang-18 llvm-18-dev python3
WORKDIR /src
COPY CMakeLists.txt .
COPY compiler/ compiler/
COPY runtime/ runtime/
COPY tests/compiler_cli.py tests/compiler_cli.py
COPY examples/compiler-smoke/ examples/compiler-smoke/
RUN cmake -S . -B /build -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 -DLLVM_DIR="$(llvm-config-18 --cmakedir)" \
    && cmake --build /build --parallel 4 && ctest --test-dir /build --output-on-failure \
    && /build/bin/lyra build examples/compiler-smoke/pass -o /out/debug \
    && /build/bin/lyra build examples/compiler-smoke/pass --release -o /out/release

FROM ubuntu:22.04 AS verify
COPY --from=builder /out/ /apps/
RUN set -eu; for app in /apps/debug /apps/release; do actual=$("$app"); test "$actual" = 3; done
