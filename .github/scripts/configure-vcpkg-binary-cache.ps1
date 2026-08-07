$ErrorActionPreference = 'Stop'

if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is required' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { throw 'VCPKG_DEFAULT_BINARY_CACHE is required' }
if (-not $env:GITHUB_TOKEN) { throw 'GITHUB_TOKEN is required' }
if (-not $env:GITHUB_REPOSITORY_OWNER) { throw 'GITHUB_REPOSITORY_OWNER is required' }

$mode = $env:VCPKG_BINARY_CACHE_MODE
if (-not $mode) {
    $mode = if ($env:GITHUB_EVENT_NAME -eq 'pull_request') { 'read' } else { 'readwrite' }
}
if ($mode -notin @('read', 'readwrite')) {
    throw "Unsupported VCPKG_BINARY_CACHE_MODE: $mode"
}

$feed = "https://nuget.pkg.github.com/$($env:GITHUB_REPOSITORY_OWNER)/index.json"
$vcpkg = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
$nuget = (& $vcpkg fetch nuget | Select-Object -Last 1).Trim()

if (-not $nuget) { throw 'vcpkg fetch nuget returned no executable path' }

& $nuget sources remove -Name GitHubPackages 2>$null
$LASTEXITCODE = 0
& $nuget sources add `
    -Source $feed `
    -StorePasswordInClearText `
    -Name GitHubPackages `
    -UserName $env:GITHUB_ACTOR `
    -Password $env:GITHUB_TOKEN
if ($mode -eq 'readwrite') {
    & $nuget setapikey $env:GITHUB_TOKEN -Source $feed
}

$sources = "clear;files,$($env:VCPKG_DEFAULT_BINARY_CACHE),$mode;nuget,$feed,$mode"
Add-Content -Path $env:GITHUB_ENV -Value "VCPKG_BINARY_SOURCES=$sources"
