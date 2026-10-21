[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PythonPath,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [string]$Requirements,

    [Parameter(Mandatory = $true)]
    [string]$GitHubPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($ExpectedVersion -notmatch '^2\.[0-9]+\.[0-9]+$') {
    throw 'The expected Conan client must be an exact Conan 2 version'
}
if (-not (Test-Path -LiteralPath $Requirements -PathType Leaf)) {
    throw 'The reviewed Conan requirements lock is missing'
}
$requirementsText = [System.IO.File]::ReadAllText($Requirements)
$expectedLine = "(?m)^conan==$([regex]::Escape($ExpectedVersion)) --hash=sha256:[0-9a-f]{64}$"
if ([regex]::Matches($requirementsText, $expectedLine).Count -ne 1 -or
    $requirementsText -match '(?m)^\s*(?:--|https?://|git\+)') {
    throw 'The Conan client version is not uniquely bound by the reviewed hash lock'
}

$venv = Join-Path $env:RUNNER_TEMP 'mcp-cpp-sdk-conan-venv'
if (Test-Path -LiteralPath $venv) {
    Remove-Item -LiteralPath $venv -Recurse -Force
}
& $PythonPath -I -m venv $venv
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to create the isolated Conan virtual environment'
}
$venvPython = Join-Path $venv 'Scripts/python.exe'
$venvScripts = Join-Path $venv 'Scripts'
& $venvPython -I -m pip install --disable-pip-version-check --no-cache-dir `
    --only-binary=:all: --require-hashes --requirement $Requirements
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install the hash-locked Conan client graph'
}
$version = (& $venvPython -I -m conan --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $version -cne "Conan version $ExpectedVersion") {
    throw 'The installed Conan client does not match the reviewed version'
}
[System.IO.File]::AppendAllText(
    $GitHubPath,
    "$venvScripts`n",
    [System.Text.UTF8Encoding]::new($false)
)
