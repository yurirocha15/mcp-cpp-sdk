[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Source,

    [Parameter(Mandatory = $true)]
    [string]$Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sourceRoot = [System.IO.Path]::GetFullPath($Source)
$destinationPath = [System.IO.Path]::GetFullPath($Destination)
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw 'Deterministic ZIP source must be a directory'
}
if (Test-Path -LiteralPath $destinationPath) {
    throw 'Deterministic ZIP destination must be new'
}

$relativePaths = [System.Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force) {
    if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Deterministic ZIP source must not contain reparse points'
    }
    $relative = [System.IO.Path]::GetRelativePath($sourceRoot, $file.FullName).Replace('\', '/')
    if (-not $relative -or $relative.StartsWith('../') -or [System.IO.Path]::IsPathRooted($relative)) {
        throw 'Deterministic ZIP entry escaped the source directory'
    }
    $relativePaths.Add($relative)
}
if ($relativePaths.Count -eq 0) {
    throw 'Deterministic ZIP source is empty'
}
$relativePaths.Sort([System.StringComparer]::Ordinal)

$parent = Split-Path -Parent $destinationPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$archiveStream = [System.IO.File]::Open(
    $destinationPath,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None
)
try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $archiveStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $false,
        [System.Text.Encoding]::UTF8
    )
    try {
        $timestamp = [System.DateTimeOffset]::new(
            1980, 1, 1, 0, 0, 0, [System.TimeSpan]::Zero
        )
        foreach ($relative in $relativePaths) {
            $entry = $archive.CreateEntry(
                $relative,
                [System.IO.Compression.CompressionLevel]::Optimal
            )
            $entry.LastWriteTime = $timestamp
            $entry.ExternalAttributes = -2119958528 # regular file, Unix mode 0644
            $entryStream = $entry.Open()
            $sourcePath = Join-Path $sourceRoot (
                $relative.Replace(
                    [char]'/',
                    [System.IO.Path]::DirectorySeparatorChar
                )
            )
            $sourceStream = [System.IO.File]::OpenRead($sourcePath)
            try {
                $sourceStream.CopyTo($entryStream)
            }
            finally {
                $sourceStream.Dispose()
                $entryStream.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $archiveStream.Dispose()
}
