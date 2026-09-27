#!/usr/bin/env bash
# Generate symbol and dependency reports for the built TinyX components.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_dir"}
build_dir=$(cd "$build_dir" && pwd)
output_dir=${2:-"$build_dir/symbol-deps"}
cd "$build_dir"

required=(
    dix/libdix.la
    kdrive/src/libkdrive.a
    fb/libfb.la
    mi/libmi.la
    xfixes/libxfixes.la
    Xext/libXext.la
    render/librender.la
    randr/librandr.la
    damageext/libdamageext.la
    miext/damage/libdamage.la
    miext/shadow/libshadow.la
    os/libos.la
    kdrive/src/libkdrivestubs.a
)

optional=(
    kdrive/fbdev/libfbdev.a
    kdrive/vesa/libvesa.a
    kdrive/linux/liblinux.a
    dbe/libdbe.la
)

missing=()
inputs=()
for file in "${required[@]}"; do
    if [[ -e $file ]]; then
        inputs+=("$file")
    else
        missing+=("$file")
    fi
done
for file in "${optional[@]}"; do
    if [[ -e $file ]]; then
        inputs+=("$file")
    fi
done

if ((${#missing[@]})); then
    printf 'error: required build artifacts are missing:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    printf '\nConfigure with --disable-lto and build TinyX before running this script.\n' >&2
    exit 1
fi

mkdir -p "$output_dir"

python3 "$repo_dir/tools/symbol-deps.py" \
    --scope archive \
    --format markdown \
    --include-symbols \
    --symbol-limit 40 \
    --output "$output_dir/archive-dependencies.md" \
    "${inputs[@]}"

python3 "$repo_dir/tools/symbol-deps.py" \
    --scope object \
    --format json \
    --output "$output_dir/object-dependencies.json" \
    "${inputs[@]}"

python3 "$repo_dir/tools/symbol-deps.py" \
    --scope archive \
    --format json \
    --output "$output_dir/archive-dependencies.json" \
    "${inputs[@]}"

printf 'wrote:\n'
printf '  %s\n' \
    "$output_dir/archive-dependencies.md" \
    "$output_dir/archive-dependencies.json" \
    "$output_dir/object-dependencies.json"
