[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Join-Path ([System.IO.Path]::GetTempPath()) (
    "mcp-cpp-sdk-zip-test-$([System.Guid]::NewGuid().ToString('N'))"
)
try {
    $source = Join-Path $root 'source'
    New-Item -ItemType Directory -Path (Join-Path $source 'nested') -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $source 'z.txt'), [byte[]](1, 2, 3))
    [System.IO.File]::WriteAllBytes((Join-Path $source 'nested/a.txt'), [byte[]](4, 5, 6))

    $first = Join-Path $root 'first.zip'
    $second = Join-Path $root 'second.zip'
    & (Join-Path $PSScriptRoot 'build_deterministic_zip.ps1') `
        -Source $source -Destination $first

    [System.IO.File]::SetLastWriteTimeUtc(
        (Join-Path $source 'z.txt'),
        [System.DateTime]::UtcNow
    )
    & (Join-Path $PSScriptRoot 'build_deterministic_zip.ps1') `
        -Source $source -Destination $second

    $firstHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $first).Hash
    $secondHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $second).Hash
    if ($firstHash -cne $secondHash) {
        throw 'Deterministic ZIP changed when source timestamps changed'
    }

    $archive = [System.IO.Compression.ZipFile]::OpenRead($first)
    try {
        $entries = @($archive.Entries)
        $names = @($entries | ForEach-Object FullName)
        if (($names -join ',') -cne 'nested/a.txt,z.txt') {
            throw 'Deterministic ZIP entries are not ordinally sorted'
        }
        foreach ($entry in $entries) {
            # ZIP stores a zone-less DOS time; .NET reads it back with the local offset.
            if ($entry.LastWriteTime.DateTime -ne [datetime]::new(1980, 1, 1) -or
                $entry.ExternalAttributes -ne -2119958528) {
                throw 'Deterministic ZIP metadata is not normalized'
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
