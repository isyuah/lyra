FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/keyrings/llvm.asc \
    && echo 'deb [signed-by=/etc/apt/keyrings/llvm.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
       build-essential binutils clang-18 clang-tools-18 llvm-18-dev libclang-18-dev lld-18 libgc-dev libuv1-dev libsqlite3-dev libjansson-dev libssl-dev zlib1g-dev openssl python3-pip \
    && python3 -m pip install --no-cache-dir --no-deps --only-binary=:all: cmake==3.30.9 ninja==1.11.1.4 \
    && clang-scan-deps-18 --version
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json VERSION ./
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

FROM ubuntu:22.04 AS smoke-base
RUN apt-get update && apt-get install -y --no-install-recommends openssl \
    && rm -rf /var/lib/apt/lists/*

FROM smoke-base AS bare-sdk
COPY --from=builder /out/sdk/ /sdk/
COPY --from=builder /src/examples/jsonlines/ /work/jsonlines/
COPY --from=builder /src/examples/http-sqlite/ /work/http-sqlite/
COPY --from=builder /out/apps/http-fixture.db /work/http-fixture.db
COPY tests/jammy-smoke.sh /verify.sh
RUN mv /sdk '/搬迁 SDK' && sh /verify.sh verify-sdk /work '/搬迁 SDK' && touch /sdk-verified

FROM smoke-base AS verify
COPY --from=bare-sdk /sdk-verified /sdk-verified
COPY --from=builder /out/apps/ /apps/
COPY tests/jammy-smoke.sh /verify.sh
RUN sh /verify.sh verify /apps

FROM builder AS sdk-package
COPY --from=verify /sdk-verified /sdk-verified
COPY tools/package_sdk.py tools/
COPY install.sh tools/install.sh
ARG LYRA_RELEASE_TAG
RUN test -n "$LYRA_RELEASE_TAG" \
    && python3 tools/package_sdk.py build/release --tag "$LYRA_RELEASE_TAG" --output /out/dist \
    && archive="$(find /out/dist -maxdepth 1 -name '*.tar.gz' -print -quit)" \
    && archive_hash="$(sha256sum "$archive" | cut -d ' ' -f 1)" \
    && mkdir -p /tmp/lyra-installer-home \
    && HOME=/tmp/lyra-installer-home sh tools/install.sh --install-dir /tmp/lyra-installer-probe --tag "$LYRA_RELEASE_TAG" --archive "$archive" --sha256 "$archive_hash" \
    && touch /tmp/lyra-installer-probe/obsolete.txt \
    && HOME=/tmp/lyra-installer-home sh tools/install.sh --install-dir /tmp/lyra-installer-probe --tag "$LYRA_RELEASE_TAG" --archive "$archive" --sha256 "$archive_hash" \
    && test ! -e /tmp/lyra-installer-probe/obsolete.txt \
    && test -n "$(find /tmp -maxdepth 2 -type f -path '/tmp/.lyra-backup-lyra-installer-probe-*/obsolete.txt' -print -quit)" \
    && test "$(/tmp/lyra-installer-probe/bin/lyra --version)" = "lyra ${LYRA_RELEASE_TAG#v}" \
    && cp tools/install.sh /out/dist/lyra-install.sh

FROM scratch AS sdk-dist
COPY --from=sdk-package /out/dist/ /
