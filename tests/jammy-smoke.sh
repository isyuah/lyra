#!/bin/sh
set -eu
export LC_ALL=C
. /etc/os-release
test "$ID:$VERSION_ID:$(uname -m)" = 'ubuntu:22.04:x86_64'

mode=${1:?expected build, verify, or verify-sdk}
apps=${2:?expected an absolute application directory}

jsonlines_input() {
    printf '%s\n' '{"station":"东区","delta":7}' '{"station":"西区","delta":-2}'
}

verify_jsonlines() { jsonlines_input | "$1"; }

verify_jsonlines_file() (
    scratch=$(mktemp -d)
    data="$scratch/记录 输入.jsonl"
    config="$scratch/统计 配置.toml"
    trap 'rm -f "$data" "$config"; rmdir "$scratch"' EXIT
    jsonlines_input > "$data"
    printf 'input = "%s"\nmaxLineBytes = 128\n' "$data" > "$config"
    "$1" --config "$config" </dev/null
    printf 'input = "%s"\nmaxLineBytes = 128\n' "$scratch/not-selected.jsonl" > "$config"
    "$1" --config "$config" --input "$data" </dev/null
)

verify_https() (
    scratch=$(mktemp -d)
    trap 'rm -f "$scratch"/*.log "$scratch"/*.db* "$scratch"/*.ready "$scratch/server.key" "$scratch/server.pem"; rmdir "$scratch"' EXIT
    cp "$2" "$scratch/upstream.db"
    cp "$2" "$scratch/service.db"
    openssl req -new -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 \
        -subj /CN=localhost -addext 'subjectAltName=DNS:localhost' -addext 'basicConstraints=critical,CA:TRUE' \
        -keyout "$scratch/server.key" -out "$scratch/server.pem" 2> "$scratch/cert.log" \
        || { cat "$scratch/cert.log" >&2; exit 1; }
    timeout 30 bash -euo pipefail -s -- "$1" "$scratch" <<'BASH'
app=$1
scratch=$2
cert="$scratch/server.pem"
key="$scratch/server.key"
pids=()
cleanup() {
    local status=$? process log
    for process in "${pids[@]}"; do kill "$process" 2>/dev/null || true; done
    for process in "${pids[@]}"; do wait "$process" 2>/dev/null || true; done
    if (( status != 0 )); then
        for log in "$scratch"/*.log; do
            if [[ -s $log ]]; then printf '\n%s:\n' "$log" >&2; cat "$log" >&2; fi
        done
    fi
}
trap cleanup EXIT
trap 'exit 124' INT TERM
start() {
    local name=$1 ready gate
    shift
    ready="$scratch/$name.ready"
    mkfifo "$ready"
    "$app" "$scratch/$name.db" --cert "$cert" --key "$key" "$@" > "$ready" 2> "$scratch/$name.log" &
    pids+=("$!")
    exec {gate}<>"$ready"
    IFS= read -r -t 5 port <&"$gate"
    [[ $port =~ ^[0-9]+$ ]]
}
request() {
    local target_port=$1 method=$2 target=$3 payload=${4-}
    {
        printf '%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n' "$method" "$target"
        if [[ $method == POST ]]; then
            printf 'Content-Type: application/json\r\nContent-Length: %s\r\n' "${#payload}"
        fi
        printf '\r\n%s' "$payload"
    } | openssl s_client -quiet -ign_eof -connect "127.0.0.1:$target_port" -servername localhost \
        -verify_hostname localhost -verify_return_error -CAfile "$cert" 2> "$scratch/client.log"
}
user_response() {
    local status=$1 id=$2 name=$3 answer=$4 body
    [[ $answer == "HTTP/1.1 $status "* && $answer == *$'\r\nContent-Type: application/json\r\n'* ]] \
        || { printf 'Unexpected HTTP response: %s\n' "$answer" >&2; return 1; }
    body=${answer#*$'\r\n\r\n'}
    [[ $body == "{\"id\":$id,\"name\":\"$name\"}" || $body == "{\"name\":\"$name\",\"id\":$id}" ]] \
        || { printf 'Unexpected user JSON: %s\n' "$body" >&2; return 1; }
}
start upstream
upstream_port=$port
answer=$(request "$upstream_port" POST /users '{"name":"Origin"}')
user_response 201 8 Origin "$answer"
upstream=$(request "$upstream_port" GET /users/8)
user_response 200 8 Origin "$upstream"
start service --upstream "https://localhost:$upstream_port/users/8" --ca "$cert"
service_port=$port
answer=$(request "$service_port" GET /users/7)
user_response 200 7 Ada "$answer"
answer=$(request "$service_port" GET /users/8)
[[ $answer == $'HTTP/1.1 404 Not Found\r\n'* ]]
answer=$(request "$service_port" POST /users '{"name":"Grace"}')
user_response 201 8 Grace "$answer"
answer=$(request "$service_port" GET /users/8)
user_response 200 8 Grace "$answer"
answer=$(request "$service_port" GET /upstream)
[[ $answer == $'HTTP/1.1 200 OK\r\n'* && ${answer#*$'\r\n\r\n'} == "${upstream#*$'\r\n\r\n'}" ]]
[[ ! -s $scratch/upstream.log && ! -s $scratch/service.log ]]
BASH
)

summary='{"rows":2,"total":5,"rejected":0}'

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
            test "$(verify_jsonlines "$output/stats")" = "$summary"
            test "$(verify_jsonlines_file "$output/stats")" = "$(printf '%s\n%s' "$summary" "$summary")"
            verify_https "$output/users" "$apps/http-fixture.db"
            printf '%s SDK: compiled and ran JSONL and HTTPS/SQLite (%s)\n' "$profile" "$PRETTY_NAME"
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
                jsonlines) prefix=jsonlines-; printf '%s\n' "$summary" > "$scratch/expected" ;;
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
            test "$(verify_jsonlines_file "$apps/jsonlines-$profile")" = "$(printf '%s\n%s' "$summary" "$summary")"
            verify_https "$app" "$apps/http-fixture.db"
            printf '%s: HTTPS, SQLite commit, and verified outbound HTTPS matched (%s)\n' "$app" "$PRETTY_NAME"
        done
        rm "$scratch/expected" "$scratch/stdout" "$scratch/stderr"
        rmdir "$scratch"
        ;;
    *) printf 'usage: %s build|verify APP_DIR | verify-sdk WORK_DIR SDK_DIR\n' "$0" >&2; exit 2 ;;
esac
