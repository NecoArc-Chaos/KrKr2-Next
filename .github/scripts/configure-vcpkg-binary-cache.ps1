$ErrorActionPreference = 'Stop'

if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is required' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { throw 'VCPKG_DEFAULT_BINARY_CACHE is required' }

$mode = $env:VCPKG_BINARY_CACHE_MODE
if (-not $mode) {
    $mode = if ($env:GITHUB_EVENT_NAME -eq 'pull_request') { 'read' } else { 'readwrite' }
}
if ($mode -notin @('read', 'readwrite')) {
    throw "Unsupported VCPKG_BINARY_CACHE_MODE: $mode"
}

$sources = "clear;files,$($env:VCPKG_DEFAULT_BINARY_CACHE),$mode"
Add-Content -Path $env:GITHUB_ENV -Value "VCPKG_BINARY_SOURCES=$sources"
