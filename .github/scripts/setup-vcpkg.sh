#!/usr/bin/env bash

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT is required}"
: "${VCPKG_DEFAULT_BINARY_CACHE:?VCPKG_DEFAULT_BINARY_CACHE is required}"

mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE" "$VCPKG_ROOT"

if [ ! -d "$VCPKG_ROOT/.git" ]; then
    git -C "$VCPKG_ROOT" init
    git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
fi

partial_clone=false
if [ "$(git -C "$VCPKG_ROOT" config --get remote.origin.promisor || true)" = true ] ||
   [ -n "$(git -C "$VCPKG_ROOT" config --get remote.origin.partialclonefilter || true)" ]; then
    partial_clone=true
fi

if [ "$partial_clone" = true ]; then
    git -C "$VCPKG_ROOT" config --unset-all remote.origin.promisor || true
    git -C "$VCPKG_ROOT" config --unset-all remote.origin.partialclonefilter || true
    fetch_args=(--refetch --no-filter --no-tags origin HEAD)
else
    fetch_args=(--no-tags origin HEAD)
fi

fetched=false
for attempt in 1 2 3; do
    if git -C "$VCPKG_ROOT" fetch "${fetch_args[@]}"; then
        fetched=true
        break
    fi
    if [ "$attempt" -lt 3 ]; then
        sleep $((attempt * 5))
    fi
done
if [ "$fetched" != true ]; then
    exit 1
fi

git -C "$VCPKG_ROOT" checkout --force FETCH_HEAD

if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

"$VCPKG_ROOT/vcpkg" version
