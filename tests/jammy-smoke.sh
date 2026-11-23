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
                profile=debug
                set --
            else
                profile=release
                set -- --release
            fi
            for sample in integer objects; do
                case "$sample" in
                    integer) package=examples/compiler-smoke/pass; app="$apps/$profile" ;;
                    objects) package=tests/fixtures/objects; app="$apps/objects-$profile" ;;
                esac
                "build/$preset/bin/lyra" build "$package" "$@" -o "$app"
                dynamic=$(readelf -dW "$app")
                needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
                printf '%s DT_NEEDED: %s\n' "$app" "$needed"
                test "$needed" = libc.so.6
            done
        done
        ;;
    verify)
        scratch=$(mktemp -d)
        for sample in integer objects; do
            case "$sample" in
                integer) prefix=; printf '3\n' > "$scratch/expected" ;;
                objects) prefix=objects-; printf '持有\nA\000B\n1\n2\n' > "$scratch/expected" ;;
            esac
            for profile in debug release; do
                app="$apps/$prefix$profile"
                "$app" > "$scratch/stdout" 2> "$scratch/stderr"
                cmp "$scratch/expected" "$scratch/stdout"
                test ! -s "$scratch/stderr"
                printf '%s: exact stdout matched, exit=0, stderr empty (%s)\n' "$app" "$PRETTY_NAME"
            done
        done
        rm "$scratch/expected" "$scratch/stdout" "$scratch/stderr"
        rmdir "$scratch"
        ;;
    *) printf 'usage: %s build|verify APP_DIR\n' "$0" >&2; exit 2 ;;
esac
