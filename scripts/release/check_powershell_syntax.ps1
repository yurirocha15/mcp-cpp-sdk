[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseScripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File -Filter '*.ps1')
if ($releaseScripts.Count -eq 0) {
    throw 'No release PowerShell scripts were found'
}

foreach ($script in $releaseScripts) {
    $tokens = $null
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $script.FullName,
        [ref]$tokens,
        [ref]$errors
    ) | Out-Null
    if ($errors.Count -ne 0) {
        $details = ($errors | ForEach-Object ToString) -join [Environment]::NewLine
        throw "PowerShell syntax errors in $($script.FullName):$([Environment]::NewLine)$details"
    }
}

Write-Host "Parsed $($releaseScripts.Count) release PowerShell scripts"
