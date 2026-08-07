$ErrorActionPreference = 'Stop'

if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is required' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { throw 'VCPKG_DEFAULT_BINARY_CACHE is required' }
if (-not $env:VCPKG_NUGET_TOKEN) { throw 'VCPKG_NUGET_TOKEN is required' }
if (-not $env:GITHUB_REPOSITORY_OWNER) { throw 'GITHUB_REPOSITORY_OWNER is required' }
if (-not $env:GITHUB_ACTOR) { throw 'GITHUB_ACTOR is required' }

$mode = $env:VCPKG_BINARY_CACHE_MODE
if (-not $mode) {
    $mode = if ($env:GITHUB_EVENT_NAME -eq 'pull_request') { 'read' } else { 'readwrite' }
}
if ($mode -notin @('read', 'readwrite')) {
    throw "Unsupported VCPKG_BINARY_CACHE_MODE: $mode"
}

$feed = "https://nuget.pkg.github.com/$($env:GITHUB_REPOSITORY_OWNER)/index.json"
$vcpkg = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
$fetchOutput = @(& $vcpkg fetch nuget 2>&1)
$fetchStatus = $LASTEXITCODE
if ($fetchStatus -ne 0) {
    $fetchOutput | ForEach-Object { Write-Error $_ }
    throw "vcpkg fetch nuget failed with exit code $fetchStatus"
}

$nuget = [string]($fetchOutput | Select-Object -Last 1)
$nuget = $nuget.Trim()

if (-not $nuget -or -not (Test-Path -LiteralPath $nuget -PathType Leaf)) {
    throw "vcpkg fetch nuget returned an invalid path: $nuget"
}

& $nuget sources remove -Name GitHubPackages 2>$null
$LASTEXITCODE = 0
& $nuget sources add `
    -Source $feed `
    -StorePasswordInClearText `
    -Name GitHubPackages `
    -UserName $env:GITHUB_ACTOR `
    -Password $env:VCPKG_NUGET_TOKEN
if ($mode -eq 'readwrite') {
    & $nuget setapikey $env:VCPKG_NUGET_TOKEN -Source $feed
}

$sources = "clear;files,$($env:VCPKG_DEFAULT_BINARY_CACHE),$mode;nuget,$feed,$mode"
Add-Content -Path $env:GITHUB_ENV -Value "VCPKG_BINARY_SOURCES=$sources"
