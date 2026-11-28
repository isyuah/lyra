#!/bin/sh
set -eu

repository=isyuah/lyra
tag=
install_dir=
archive_path=
expected_sha256=

usage() {
    printf '%s\n' 'Usage: install.sh [--install-dir <directory>] [--tag vX.Y.Z]' \
        '       install.sh --install-dir <directory> --tag vX.Y.Z --archive <tar.gz> --sha256 <hash>'
}

fail() {
    printf 'Lyra installer: %s\n' "$1" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --install-dir)
            [ "$#" -ge 2 ] || fail 'missing value for --install-dir'
            install_dir=$2
            shift 2
            ;;
        --tag)
            [ "$#" -ge 2 ] || fail 'missing value for --tag'
            tag=$2
            shift 2
            ;;
        --archive)
            [ "$#" -ge 2 ] || fail 'missing value for --archive'
            archive_path=$2
            shift 2
            ;;
        --sha256)
            [ "$#" -ge 2 ] || fail 'missing value for --sha256'
            expected_sha256=$2
            shift 2
            ;;
        --repository)
            [ "$#" -ge 2 ] || fail 'missing value for --repository'
            repository=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *) fail "unknown option: $1" ;;
    esac
done

case "$(uname -s)" in Linux) ;; *) fail 'the Linux SDK installer only supports Linux.' ;; esac
case "$(uname -m)" in x86_64|amd64) ;; *) fail 'the Linux SDK installer requires x86-64.' ;; esac
printf '%s\n' "$repository" | grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' || fail "invalid GitHub repository: $repository"
command -v curl >/dev/null 2>&1 || fail 'curl is required.'
command -v sha256sum >/dev/null 2>&1 || fail 'sha256sum is required.'
command -v tar >/dev/null 2>&1 || fail 'tar is required.'

if [ -n "$archive_path" ]; then
    [ -n "$tag" ] && [ -n "$expected_sha256" ] || fail 'local archives require --tag and --sha256.'
    [ -f "$archive_path" ] || fail "SDK archive not found: $archive_path"
    case "$expected_sha256" in *[!0-9A-Fa-f]*|'') fail 'expected a 64-character SHA-256 hash.' ;; esac
    [ "${#expected_sha256}" -eq 64 ] || fail 'expected a 64-character SHA-256 hash.'
    archive_path=$(cd "$(dirname "$archive_path")" && pwd -P)/$(basename "$archive_path")
else
    [ -z "$expected_sha256" ] || fail '--sha256 is only used with --archive.'
    if [ -z "$tag" ]; then
        release_json=$(mktemp "${TMPDIR:-/tmp}/lyra-release.XXXXXX") || fail 'could not create a temporary file.'
        if ! curl -fsSL -H 'Accept: application/vnd.github+json' \
            "https://api.github.com/repos/$repository/releases?per_page=1" -o "$release_json"; then
            rm -f -- "$release_json"
            fail 'could not query GitHub releases.'
        fi
        tag=$(sed -n 's/^[[:space:]]*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' "$release_json" | sed -n '1p')
        rm -f -- "$release_json"
    fi
fi

printf '%s\n' "$tag" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$' || fail "invalid release tag: $tag"
version=${tag#v}
case "$version" in *[!0-9A-Za-z.-]*|'') fail "invalid release version: $version" ;; esac

asset_name="lyra-$version-ubuntu22.04-x86_64.tar.gz"
if [ -z "$install_dir" ]; then
    existing=$(command -v lyra 2>/dev/null || true)
    if [ -n "$existing" ]; then
        existing=$(readlink -f -- "$existing" 2>/dev/null || printf '%s' "$existing")
        candidate=$(dirname "$(dirname "$existing")")
        if [ -f "$candidate/share/lyra/sdk-sha256.txt" ]; then install_dir=$candidate; fi
    fi
fi
if [ -z "$install_dir" ]; then install_dir=${HOME:?HOME is not set}/.local/opt/lyra; fi
case "$install_dir" in /*) ;; *) install_dir=$PWD/$install_dir ;; esac
install_parent=$(dirname "$install_dir")
install_name=$(basename "$install_dir")
[ "$install_name" != . ] && [ "$install_name" != .. ] || fail "unsafe SDK install directory: $install_dir"
mkdir -p -- "$install_parent"
install_parent=$(cd "$install_parent" && pwd -P)
install_dir=$install_parent/$install_name
home_dir=$(cd "${HOME:?HOME is not set}" && pwd -P)
case "$install_dir" in /|/usr|/usr/bin|/usr/local|/bin|/sbin|/etc|/opt|/var|/home|"$home_dir") \
    fail "refusing to replace a system or home directory: $install_dir" ;; esac
[ ! -e "$install_dir" ] || [ -d "$install_dir" ] || fail "the SDK install path is a file: $install_dir"
if [ -d "$install_dir" ] && { [ ! -f "$install_dir/bin/lyra" ] || [ ! -f "$install_dir/share/lyra/sdk-sha256.txt" ]; }; then
    fail "the existing install directory is not a Lyra SDK: $install_dir"
fi

work=$(mktemp -d "$install_parent/.lyra-install.XXXXXX") || fail 'could not create a staging directory beside the install path.'
stage=$work/extracted
mkdir "$stage"
backup=
backup_active=false
installed_ok=false
cleanup() {
    if [ "$installed_ok" != true ] && [ "$backup_active" = true ] && [ -d "$backup" ]; then
        if [ -e "$install_dir" ]; then mv -- "$install_dir" "$work/failed-install" || printf 'The new SDK could not be moved aside; previous SDK remains at %s\n' "$backup" >&2; fi
        if [ ! -e "$install_dir" ]; then
            if mv -- "$backup" "$install_dir"; then backup_active=false; else printf 'Previous SDK remains at %s\n' "$backup" >&2; fi
        fi
    fi
    case "$work" in "$install_parent"/.lyra-install.*) rm -rf -- "$work" ;; esac
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

if [ -z "$archive_path" ]; then
    base_uri="https://github.com/$repository/releases/download/$tag"
    archive_path=$work/$asset_name
    sums=$work/SHA256SUMS
    curl -fsSL "$base_uri/$asset_name" -o "$archive_path" || fail "could not download $asset_name."
    curl -fsSL "$base_uri/SHA256SUMS" -o "$sums" || fail 'could not download SHA256SUMS.'
    expected_sha256=$(awk -v name="$asset_name" '$2 == name { print $1 }' "$sums" | sed -n '1p')
    [ -n "$expected_sha256" ] || fail "SHA256SUMS does not contain $asset_name."
fi

case "$expected_sha256" in *[!0-9A-Fa-f]*|'') fail 'invalid SHA-256 hash.' ;; esac
[ "${#expected_sha256}" -eq 64 ] || fail 'invalid SHA-256 hash.'
printf '%s  %s\n' "$expected_sha256" "$archive_path" | sha256sum -c --status || fail 'the SDK archive failed its SHA-256 check.'

tar -tzf "$archive_path" > "$work/archive-files" || fail 'the SDK archive is invalid.'
awk '$0 ~ /(^|\/)\.\.($|\/)/ || $0 ~ /^\// { unsafe = 1 } END { exit unsafe }' "$work/archive-files" || fail 'the SDK archive contains an unsafe path.'
tar -xzf "$archive_path" -C "$stage" --no-same-owner --no-same-permissions || fail 'could not extract the SDK archive.'
set -- "$stage"/*
[ "$#" -eq 1 ] && [ -d "$1" ] || fail 'the SDK archive must contain exactly one top-level directory.'
payload=$1
expected_root="lyra-$version-ubuntu22.04-x86_64"
[ "$(basename "$payload")" = "$expected_root" ] || fail "unexpected SDK directory: $(basename "$payload")"

binary=$payload/bin/lyra
manifest=$payload/share/lyra/sdk-sha256.txt
[ -f "$binary" ] && [ -f "$manifest" ] || fail 'the SDK is missing its executable or file manifest.'
manifest_has_binary=false
while IFS=' ' read -r file_hash relative; do
    case "$file_hash" in *[!0-9A-Fa-f]*|'') fail 'the SDK file manifest is malformed.' ;; esac
    [ "${#file_hash}" -eq 64 ] || fail 'the SDK file manifest is malformed.'
    case "$relative" in ''|/*|../*|*/../*|*/..) fail "unsafe path in SDK manifest: $relative" ;; esac
    [ -f "$payload/$relative" ] || fail "the SDK manifest refers to a missing file: $relative"
    (cd "$payload" && printf '%s  %s\n' "$file_hash" "$relative" | sha256sum -c --status) || fail "SDK file failed its SHA-256 check: $relative"
    if [ "$relative" = bin/lyra ]; then manifest_has_binary=true; fi
done < "$manifest"
[ "$manifest_has_binary" = true ] || fail 'the SDK manifest does not cover bin/lyra.'

actual_version=$("$binary" --version) || fail 'the SDK executable could not run.'
[ "$actual_version" = "lyra $version" ] || fail "expected 'lyra $version', got '$actual_version'."

if [ -e "$install_dir" ]; then
    backup=$install_parent/.lyra-backup-$install_name-$$
    [ ! -e "$backup" ] || fail "backup path already exists: $backup"
    mv -- "$install_dir" "$backup" || fail "could not move the existing SDK: $install_dir"
    backup_active=true
fi
if ! mv -- "$payload" "$install_dir"; then
    if [ "$backup_active" = true ] && [ -d "$backup" ]; then mv -- "$backup" "$install_dir" || fail "installation failed and the previous SDK remains at $backup"; backup_active=false; fi
    fail 'could not move the new SDK into place.'
fi
if ! installed_version=$("$install_dir/bin/lyra" --version) || [ "$installed_version" != "lyra $version" ]; then
    mv -- "$install_dir" "$work/failed-install" || fail "installation check failed; remove $install_dir and restore $backup manually."
    if [ "$backup_active" = true ] && [ -d "$backup" ]; then mv -- "$backup" "$install_dir" || fail "installation check failed; previous SDK remains at $backup"; backup_active=false; fi
    fail 'the installed compiler did not pass its version check.'
fi
installed_ok=true
if [ "$backup_active" = true ] && [ -d "$backup" ]; then
    case "$backup" in
        "$install_parent"/.lyra-backup-"$install_name"-*)
            previous_manifest=$backup/share/lyra/sdk-sha256.txt
            expected_files=$(wc -l < "$previous_manifest")
            actual_files=$(find "$backup" -type f ! -path "$previous_manifest" | wc -l)
            if [ "$expected_files" -eq "$actual_files" ] && (cd "$backup" && sha256sum --check --quiet share/lyra/sdk-sha256.txt); then
                if rm -rf -- "$backup"; then backup_active=false; else printf 'Lyra installed; the previous SDK remains at %s\n' "$backup" >&2; fi
            else
                printf 'The previous directory contained modified or unmanaged files; it was preserved at %s.\n' "$backup" >&2
            fi
            ;;
    esac
fi

user_bin=${HOME:?HOME is not set}/.local/bin
if mkdir -p -- "$user_bin"; then
    link=$user_bin/lyra
    if [ -e "$link" ] && [ ! -L "$link" ]; then
        printf 'Lyra installed to %s, but %s is an existing non-symlink; PATH was not changed.\n' "$install_dir" "$link" >&2
    else
        link_stage=$user_bin/.lyra-install-$$
        if ln -s -- "$install_dir/bin/lyra" "$link_stage" && mv -f -- "$link_stage" "$link"; then
            :
        else
            rm -f -- "$link_stage"
            printf 'Lyra installed to %s, but the command link could not be updated.\n' "$install_dir" >&2
        fi
    fi
else
    printf 'Lyra installed to %s, but %s could not be created.\n' "$install_dir" "$user_bin" >&2
fi

printf 'Installed Lyra %s to %s\n' "$version" "$install_dir"
case ":${PATH:-}:" in *":$user_bin:"*) ;; *) printf 'Add Lyra to PATH with: export PATH="%s:$PATH"\n' "$user_bin" ;; esac
