#!/bin/sh
set -eu
export LC_ALL=C
. /etc/os-release
test "$ID:$VERSION_ID:$(uname -m)" = 'ubuntu:22.04:x86_64'

mode=${1:?expected build, verify, or verify-sdk}
apps=${2:?expected an absolute application directory}

verify_jsonlines() {
    printf '%s\n' '{"station":"东区","delta":7}' '{"station":"西区","delta":-2}' | "$1"
}

verify_http() {
    timeout 15 bash -euo pipefail -s -- "$1" "$2" <<'BASH'
coproc service { exec stdbuf -oL "$1" "$2"; }
pid=$service_PID
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
IFS= read -r -t 5 port <&"${service[0]}"
request() {
    exec {client}<>"/dev/tcp/127.0.0.1/$port"
    printf 'GET /users/%s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' "$1" >&"$client"
    cat <&"$client"
    exec {client}>&-
}
answer=$(request 7)
[[ $answer == $'HTTP/1.1 200 OK\r\n'* && $answer == *$'\r\nContent-Type: application/json\r\n'* ]]
body=$(printf '%s' "${answer#*$'\r\n\r\n'}" | tr -d '[:space:]')
[[ $body == '{"id":7,"name":"Ada"}' || $body == '{"name":"Ada","id":7}' ]]
answer=$(request 8)
[[ $answer == $'HTTP/1.1 404 Not Found\r\n'* ]]
BASH
}

case "$mode" in
    build)
        mkdir -p "$apps"
        python3 - "$apps/http-fixture.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(":memory:") as fixture, sqlite3.connect(sys.argv[1]) as output:
    fixture.executescript("CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT); INSERT INTO users VALUES(7,'Ada');")
    fixture.backup(output)
PY
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
            for sample in integer objects stdio sqlite jsonlines http; do
                case "$sample" in
                    integer) package=examples/compiler-smoke/pass; app="$apps/$profile" ;;
                    objects) package=tests/fixtures/objects; app="$apps/objects-$profile" ;;
                    jsonlines) package=examples/jsonlines/stats; app="$apps/jsonlines-$profile" ;;
                    http) package=examples/http-sqlite; app="$apps/http-$profile" ;;
                    stdio|sqlite) package="tests/fixtures/$sample"; app="$apps/$sample-$profile" ;;
                esac
                "build/$preset/bin/lyra" build "$package" "$@" -o "$app"
                dynamic=$(readelf -dW "$app")
                needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p')
                printf '%s DT_NEEDED: %s\n' "$app" "$needed"
                if [ "$sample" = sqlite ] || [ "$sample" = http ]; then
                    test "$(printf '%s\n' "$needed" | sort)" = "$(printf '%s\n' libc.so.6 libm.so.6)"
                else
                    test "$needed" = libc.so.6
                fi
            done
        done
        ;;
    verify-sdk)
        sdk=${3:?expected a relocated SDK directory}
        lyra() { env -i LC_ALL=C.UTF-8 PATH= "LYRA_CACHE=$apps/private cache" "$sdk/bin/lyra" "$@"; }
        hello="$apps/中文 项目"
        lyra new "$hello" --name hello
        lyra lock "$hello" --offline
        lyra test "$hello" --locked --offline
        test "$(lyra run "$hello" --release --locked --offline)" = 42
        lyra test "$apps/jsonlines" --locked --offline
        for profile in debug release; do
            set --
            if [ "$profile" = release ]; then set -- --release; fi
            output="$apps/isolated builds/$profile"
            lyra build "$apps/jsonlines/stats" "$@" --locked --offline -o "$output/stats"
            lyra build "$apps/http-sqlite" "$@" --locked --offline -o "$output/users"
            test "$(verify_jsonlines "$output/stats")" = '{"rows":2,"total":5,"rejected":0}'
            verify_http "$output/users" "$apps/http-fixture.db"
            printf '%s SDK: compiled and ran JSONL and HTTP/SQLite (%s)\n' "$profile" "$PRETTY_NAME"
        done
        ;;
    verify)
        scratch=$(mktemp -d)
        for sample in integer objects stdio jsonlines sqlite; do
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
                    verify_jsonlines "$app" > "$scratch/stdout" 2> "$scratch/stderr"
                else
                    "$app" > "$scratch/stdout" 2> "$scratch/stderr"
                fi
                cmp "$scratch/expected" "$scratch/stdout"
                test ! -s "$scratch/stderr"
                printf '%s: exact stdout matched, exit=0, stderr empty (%s)\n' "$app" "$PRETTY_NAME"
            done
        done
        for profile in debug release; do
            app="$apps/http-$profile"
            verify_http "$app" "$apps/http-fixture.db" 2> "$scratch/stderr"
            test ! -s "$scratch/stderr"
            printf '%s: HTTP 200 and 404 matched (%s)\n' "$app" "$PRETTY_NAME"
        done
        rm "$scratch/expected" "$scratch/stdout" "$scratch/stderr"
        rmdir "$scratch"
        ;;
    *) printf 'usage: %s build|verify APP_DIR | verify-sdk WORK_DIR SDK_DIR\n' "$0" >&2; exit 2 ;;
esac
