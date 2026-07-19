[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Preflight', 'Publish')]
    [string]$Mode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RequiredEnvironment {
    param([Parameter(Mandatory = $true)][string]$Name)

    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Required publisher input is missing: $Name"
    }
    return $value
}

function Get-TrustedChocolatey {
    $expectedSignerSubject = 'CN="Chocolatey Software, Inc", O="Chocolatey Software, Inc", L=Topeka, S=Kansas, C=US'
    $expectedSignerThumbprint = 'B009C875F4E10FFBC62B785BAF4FC4D6BC2D5711'
    $expectedVersion = Get-RequiredEnvironment -Name 'EXPECTED_CHOCOLATEY_VERSION'
    if ($expectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw 'Expected Chocolatey version is not canonical'
    }
    $installRoot = Get-RequiredEnvironment -Name 'ChocolateyInstall'
    $choco = [System.IO.Path]::GetFullPath((Join-Path $installRoot 'bin/choco.exe'))
    $item = Get-Item -LiteralPath $choco -ErrorAction Stop
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Preinstalled Chocolatey CLI must not be a reparse point'
    }
    $command = Get-Command -Name 'choco.exe' -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    if (-not [System.IO.Path]::GetFullPath($command.Source).Equals(
            $choco,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
        throw 'PATH does not resolve to the trusted preinstalled Chocolatey CLI'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $choco
    if ($signature.Status -cne 'Valid' -or
        $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -cne $expectedSignerSubject -or
        $signature.SignerCertificate.Thumbprint -cne $expectedSignerThumbprint) {
        throw 'Preinstalled Chocolatey CLI Authenticode identity is not approved'
    }
    $actualVersion = (& $choco --version).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualVersion -cne $expectedVersion) {
        throw 'Preinstalled Chocolatey CLI does not match the reviewed version'
    }
    return $choco
}

function Assert-Digest {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ($Expected -notmatch '^[0-9a-f]{64}$') {
        throw "$Label expected SHA-256 is malformed"
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $Expected) {
        throw "$Label SHA-256 mismatch"
    }
}

$clientPath = [System.IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
Assert-Digest `
    -Path $clientPath `
    -Expected (Get-RequiredEnvironment -Name 'EXPECTED_PUBLISHER_CLIENT_SHA256') `
    -Label 'Publisher client'
$choco = Get-TrustedChocolatey
if ($Mode -eq 'Preflight') {
    return
}

$submission = Split-Path -Parent $clientPath
$packageName = Get-RequiredEnvironment -Name 'EXPECTED_PACKAGE'
if ($packageName -notmatch '^mcp-cpp-sdk\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.nupkg$') {
    throw 'Expected Chocolatey package name is not canonical'
}
$expectedInventory = @('identity.json', $packageName, 'push_package.ps1') | Sort-Object
$actualEntries = @(Get-ChildItem -LiteralPath $submission -Force)
$actualInventory = @($actualEntries | ForEach-Object Name | Sort-Object)
$inventoryDifference = @(Compare-Object $expectedInventory $actualInventory)
$unsafeEntries = @($actualEntries | Where-Object {
        $_.PSIsContainer -or
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    })
if ($inventoryDifference.Count -ne 0 -or $unsafeEntries.Count -ne 0) {
    throw 'Submission handoff inventory mismatch'
}

$identityPath = Join-Path $submission 'identity.json'
Assert-Digest `
    -Path $identityPath `
    -Expected (Get-RequiredEnvironment -Name 'EXPECTED_IDENTITY_SHA256') `
    -Label 'Submission identity'
$identity = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedFields = @(
    'package_name', 'package_sha256', 'publisher_client_name',
    'publisher_client_sha256', 'release_manifest_sha256', 'request_uuid',
    'run_attempt', 'run_id', 'schema_version', 'source_commit_sha', 'source_tag'
) | Sort-Object
$actualFields = @($identity.PSObject.Properties.Name | Sort-Object)
if (Compare-Object $expectedFields $actualFields) {
    throw 'Submission handoff schema mismatch'
}
if ($identity.schema_version -ne 2 -or
    $identity.source_tag -cne (Get-RequiredEnvironment -Name 'EXPECTED_TAG') -or
    $identity.source_commit_sha -cne (Get-RequiredEnvironment -Name 'EXPECTED_COMMIT') -or
    $identity.release_manifest_sha256 -cne (Get-RequiredEnvironment -Name 'EXPECTED_MANIFEST') -or
    $identity.request_uuid -cne (Get-RequiredEnvironment -Name 'EXPECTED_REQUEST_UUID') -or
    $identity.run_id -cne (Get-RequiredEnvironment -Name 'RUN_ID') -or
    $identity.run_attempt -cne (Get-RequiredEnvironment -Name 'RUN_ATTEMPT') -or
    $identity.package_name -cne $packageName -or
    $identity.package_sha256 -cne (Get-RequiredEnvironment -Name 'EXPECTED_SHA256') -or
    $identity.publisher_client_name -cne 'push_package.ps1' -or
    $identity.publisher_client_sha256 -cne (Get-RequiredEnvironment -Name 'EXPECTED_PUBLISHER_CLIENT_SHA256')) {
    throw 'Submission handoff identity mismatch'
}

$package = Join-Path $submission $packageName
Assert-Digest `
    -Path $package `
    -Expected (Get-RequiredEnvironment -Name 'EXPECTED_SHA256') `
    -Label 'Submission package'
$apiKey = Get-RequiredEnvironment -Name 'CHOCOLATEY_API_KEY'
if ($apiKey.Length -gt 512 -or $apiKey -match '[\x00-\x20\x7f]') {
    throw 'Chocolatey API key has an unsafe shape'
}
try {
    & $choco push $package `
        --source 'https://push.chocolatey.org/' `
        --api-key $apiKey `
        --limit-output
    if ($LASTEXITCODE -ne 0) {
        throw "Chocolatey push failed with exit code $LASTEXITCODE"
    }
}
finally {
    $apiKey = $null
    $env:CHOCOLATEY_API_KEY = $null
}
