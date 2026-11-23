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
            for sample in integer objects stdio sqlite jsonlines; do
                case "$sample" in
                    integer) package=examples/compiler-smoke/pass; app="$apps/$profile" ;;
                    objects) package=tests/fixtures/objects; app="$apps/objects-$profile" ;;
                    jsonlines) package=examples/jsonlines/stats; app="$apps/jsonlines-$profile" ;;
                    stdio|sqlite) package="tests/fixtures/$sample"; app="$apps/$sample-$profile" ;;
                esac
                "build/$preset/bin/lyra" build "$package" "$@" -o "$app"
                dynamic=$(readelf -dW "$app")
                needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
                printf '%s DT_NEEDED: %s\n' "$app" "$needed"
                if [ "$sample" = sqlite ]; then
                    test "$(printf '%s\n' "$needed" | sort)" = "$(printf '%s\n' libc.so.6 libsqlite3.so.0)"
                else
                    test "$needed" = libc.so.6
                fi
            done
        done
        ;;
    verify|verify-sqlite)
        scratch=$(mktemp -d)
        if [ "$mode" = verify ]; then samples='integer objects stdio jsonlines'; else samples=sqlite; fi
        for sample in $samples; do
            case "$sample" in
                integer) prefix=; printf '3\n' > "$scratch/expected" ;;
                objects) prefix=objects-; printf '持有\nA\000B\n1\n2\n' > "$scratch/expected" ;;
                stdio) prefix=stdio-; printf '输入\n' > "$scratch/expected" ;;
                sqlite) prefix=sqlite-; printf 'SQLite: 文本\n' > "$scratch/expected" ;;
                jsonlines) prefix=jsonlines-; printf '%s\n' '{"rows":2,"total":5,"rejected":0}' > "$scratch/expected" ;;
            esac
            for profile in debug release; do
                app="$apps/$prefix$profile"
                if [ "$sample" = stdio ]; then
                    "$app" "$scratch/file" < "$scratch/expected" > "$scratch/stdout" 2> "$scratch/stderr"
                    rm "$scratch/file"
                elif [ "$sample" = jsonlines ]; then
                    printf '%s\n' '{"station":"东区","delta":7}' '{"station":"西区","delta":-2}' | "$app" > "$scratch/stdout" 2> "$scratch/stderr"
                else
                    "$app" > "$scratch/stdout" 2> "$scratch/stderr"
                fi
                cmp "$scratch/expected" "$scratch/stdout"
                test ! -s "$scratch/stderr"
                printf '%s: exact stdout matched, exit=0, stderr empty (%s)\n' "$app" "$PRETTY_NAME"
            done
        done
        rm "$scratch/expected" "$scratch/stdout" "$scratch/stderr"
        rmdir "$scratch"
        ;;
    *) printf 'usage: %s build|verify|verify-sqlite APP_DIR\n' "$0" >&2; exit 2 ;;
esac
