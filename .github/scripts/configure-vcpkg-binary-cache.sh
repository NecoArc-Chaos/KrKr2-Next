#!/usr/bin/env bash

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT is required}"
: "${VCPKG_DEFAULT_BINARY_CACHE:?VCPKG_DEFAULT_BINARY_CACHE is required}"

mode="${VCPKG_BINARY_CACHE_MODE:-}"
if [ -z "$mode" ]; then
    if [ "${GITHUB_EVENT_NAME:-}" = pull_request ]; then
        mode=read
    else
        mode=readwrite
    fi
fi
case "$mode" in
    read|readwrite) ;;
    *)
        echo "Unsupported VCPKG_BINARY_CACHE_MODE: $mode" >&2
        exit 2
        ;;
esac

printf 'VCPKG_BINARY_SOURCES=clear;files,%s,%s\n' \
    "$VCPKG_DEFAULT_BINARY_CACHE" "$mode" >> "$GITHUB_ENV"
