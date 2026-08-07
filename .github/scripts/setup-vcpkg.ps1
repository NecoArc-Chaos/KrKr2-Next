$ErrorActionPreference = 'Stop'

if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is required' }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) { throw 'VCPKG_DEFAULT_BINARY_CACHE is required' }

New-Item -ItemType Directory -Force -Path $env:VCPKG_DEFAULT_BINARY_CACHE, $env:VCPKG_ROOT | Out-Null
$gitDir = Join-Path $env:VCPKG_ROOT '.git'
if (-not (Test-Path -LiteralPath $gitDir -PathType Container)) {
    git -C $env:VCPKG_ROOT init
    git -C $env:VCPKG_ROOT remote add origin https://github.com/microsoft/vcpkg.git
}

$promisor = (& git -C $env:VCPKG_ROOT config --get remote.origin.promisor 2>$null | Out-String).Trim()
$filter = (& git -C $env:VCPKG_ROOT config --get remote.origin.partialclonefilter 2>$null | Out-String).Trim()
$partialClone = $promisor -eq 'true' -or -not [string]::IsNullOrWhiteSpace($filter)
$fetchArgs = @('--no-tags', 'origin', 'HEAD')
if ($partialClone) {
    & git -C $env:VCPKG_ROOT config --unset-all remote.origin.promisor 2>$null
    $LASTEXITCODE = 0
    & git -C $env:VCPKG_ROOT config --unset-all remote.origin.partialclonefilter 2>$null
    $LASTEXITCODE = 0
    $fetchArgs = @('--refetch', '--no-filter', '--no-tags', 'origin', 'HEAD')
}
$fetched = $false
for ($attempt = 1; $attempt -le 3; $attempt++) {
    & git -C $env:VCPKG_ROOT fetch @fetchArgs
    if ($LASTEXITCODE -eq 0) {
        $fetched = $true
        break
    }
    if ($attempt -lt 3) {
        Start-Sleep -Seconds ($attempt * 5)
    }
}
if (-not $fetched) { throw 'Unable to fetch the complete vcpkg repository.' }

& git -C $env:VCPKG_ROOT checkout --force FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw 'Unable to check out the fetched vcpkg repository.' }

$vcpkg = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkg -PathType Leaf)) {
    & (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'Unable to bootstrap vcpkg.' }
}

& $vcpkg version
