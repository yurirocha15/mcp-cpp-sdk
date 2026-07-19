[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$CandidateDirectory,

    [Parameter(Mandatory = $true)]
    [string]$WorkDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Evidence,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedConanVersion,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVCToolsVersion,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1930, 1949)]
    [int]$ExpectedMscVer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw 'ConanCenter candidate validation accepts stable versions only'
}
if ($ExpectedConanVersion -notmatch '^2\.[0-9]+\.[0-9]+$') {
    throw 'ConanCenter candidate validation requires an exact Conan 2 version'
}
if ($ExpectedVCToolsVersion -notmatch '^14\.(?:3|4)[0-9]\.[0-9]+$') {
    throw 'ConanCenter candidate validation requires an exact VS 2022 toolset'
}
if (-not $env:VCToolsVersion -or
    $env:VCToolsVersion.Trim().TrimEnd('\') -cne $ExpectedVCToolsVersion) {
    throw 'The active Visual C++ tools differ from the reviewed release pin'
}
if ($env:VisualStudioVersion -notmatch '^17\.' -or
    $env:VSCMD_ARG_TGT_ARCH -cne 'x64' -or
    $env:VSCMD_ARG_HOST_ARCH -cne 'x64' -or
    $env:PROCESSOR_ARCHITECTURE -cne 'AMD64') {
    throw 'ConanCenter candidate validation requires native x64 VS 2022'
}
if (Test-Path -LiteralPath $WorkDirectory) {
    throw 'ConanCenter candidate work directory must be new'
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$candidateRoot = [System.IO.Path]::GetFullPath($CandidateDirectory)
$archive = Join-Path $candidateRoot "mcp-cpp-sdk-$Version.tar.gz"
$profile = Join-Path $repoRoot 'release/windows/conan-msvc-release.profile'
$lockfile = Join-Path $repoRoot 'release/windows/conan.lock'
foreach ($path in @($archive, $profile, $lockfile)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "ConanCenter candidate input is missing: $path"
    }
}
$profileCompilerVersion = if ($ExpectedMscVer -ge 1940) { '194' } else { '193' }
$profileText = [System.IO.File]::ReadAllText($profile)
if ($profileText -notmatch "(?m)^compiler\.version=$profileCompilerVersion$") {
    throw 'The reviewed ConanCenter profile conflicts with the expected MSVC series'
}
$python = (Get-Command python.exe -CommandType Application -ErrorAction Stop).Source
$conan = (Get-Command conan.exe -CommandType Application -ErrorAction Stop).Source
$cl = (Get-Command cl.exe -CommandType Application -ErrorAction Stop).Source

$probeRoot = Join-Path $env:RUNNER_TEMP 'mcp-cpp-sdk-conan-msvc-probe'
if (Test-Path -LiteralPath $probeRoot) {
    Remove-Item -LiteralPath $probeRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $probeRoot | Out-Null
$probeSource = Join-Path $probeRoot 'probe.cpp'
$probeExecutable = Join-Path $probeRoot 'probe.exe'
[System.IO.File]::WriteAllText(
    $probeSource,
    "#include <iostream>`nint main() { std::cout << _MSC_VER; }`n",
    [System.Text.Encoding]::ASCII
)
try {
    & $cl /nologo /EHsc /O2 /MD /DNDEBUG $probeSource "/Fe$probeExecutable"
    if ($LASTEXITCODE -ne 0) {
        throw 'MSVC identity probe compilation failed'
    }
    $actualMscVer = (& $probeExecutable 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualMscVer -cne [string]$ExpectedMscVer) {
        throw 'MSVC identity probe differs from the reviewed release pin'
    }

    & $python -I -S scripts/run_release_tool.py release.conan_validation `
        --assets $candidateRoot `
        --archive $archive `
        --version $Version `
        --work $WorkDirectory `
        --conan $conan `
        --expected-conan-version $ExpectedConanVersion `
        --build-profile $profile `
        --host-profile $profile `
        --lockfile $lockfile `
        --evidence $Evidence
    if ($LASTEXITCODE -ne 0) {
        throw 'Exact Windows ConanCenter recipe validation failed'
    }
}
finally {
    if (Test-Path -LiteralPath $probeRoot) {
        Remove-Item -LiteralPath $probeRoot -Recurse -Force
    }
}
