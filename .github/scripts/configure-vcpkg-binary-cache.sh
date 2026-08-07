#!/usr/bin/env bash

set -euo pipefail

: "${VCPKG_ROOT:?VCPKG_ROOT is required}"
: "${VCPKG_DEFAULT_BINARY_CACHE:?VCPKG_DEFAULT_BINARY_CACHE is required}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${GITHUB_REPOSITORY_OWNER:?GITHUB_REPOSITORY_OWNER is required}"

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

feed="https://nuget.pkg.github.com/${GITHUB_REPOSITORY_OWNER}/index.json"
nuget="$(${VCPKG_ROOT}/vcpkg fetch nuget | tail -n 1)"

test -n "$nuget"
mono "$nuget" sources remove -Name GitHubPackages >/dev/null 2>&1 || true
mono "$nuget" sources add \
    -Source "$feed" \
    -StorePasswordInClearText \
    -Name GitHubPackages \
    -UserName "${GITHUB_ACTOR}" \
    -Password "$GITHUB_TOKEN"
if [ "$mode" = readwrite ]; then
    mono "$nuget" setapikey "$GITHUB_TOKEN" -Source "$feed"
fi

printf 'VCPKG_BINARY_SOURCES=clear;files,%s,%s;nuget,%s,%s\n' \
    "$VCPKG_DEFAULT_BINARY_CACHE" "$mode" "$feed" "$mode" >> "$GITHUB_ENV"
