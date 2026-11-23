#!/bin/sh
set -eu
export LC_ALL=C
. /etc/os-release
test "$ID:$VERSION_ID:$(uname -m)" = 'ubuntu:22.04:x86_64'

mode=${1:?expected build or verify}
apps=${2:?expected an absolute application directory}
case "$mode" in
    build)
        mkdir -p "$apps"
        for preset in dev release; do
            cmake --preset "$preset" -DLLVM_DIR="$(llvm-config-18 --cmakedir)"
            cmake --build --preset "$preset" --parallel 4
            ctest --preset "$preset" --no-tests=error
            test -s "build/$preset/lib/liblyraRuntime.a"
            if [ "$preset" = dev ]; then
                app="$apps/debug"
                build/dev/bin/lyra build examples/compiler-smoke/pass -o "$app"
            else
                app="$apps/release"
                build/release/bin/lyra build examples/compiler-smoke/pass --release -o "$app"
            fi
            dynamic=$(readelf -dW "$app")
            needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
            printf '%s DT_NEEDED: %s\n' "$app" "$needed"
            test "$needed" = libc.so.6
        done
        ;;
    verify)
        scratch=$(mktemp -d)
        printf '3\n' > "$scratch/expected"
        for app in "$apps/debug" "$apps/release"; do
            "$app" > "$scratch/stdout" 2> "$scratch/stderr"
            cmp "$scratch/expected" "$scratch/stdout"
            test ! -s "$scratch/stderr"
            printf '%s: stdout=3\\n, exit=0, stderr empty (%s)\n' "$app" "$PRETTY_NAME"
        done
        rm "$scratch/expected" "$scratch/stdout" "$scratch/stderr"
        rmdir "$scratch"
        ;;
    *) printf 'usage: %s build|verify APP_DIR\n' "$0" >&2; exit 2 ;;
esac
