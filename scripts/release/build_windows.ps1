[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$Tag,

    [Parameter(Mandatory = $true)]
    [string]$Repository,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedCMakeVersion,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedConanVersion,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedChocolateyVersion,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedPythonVersion,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVCToolsVersion,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1930, 1949)]
    [int]$ExpectedMscVer
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

function Invoke-CapturedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$FailureMessage
    )

    $output = & $FilePath @Arguments 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
    return $output.Trim()
}

function Get-RequiredApplication {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $command = Get-Command -Name $Name -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw "Required release tool is unavailable: $Name"
    }
    return $command.Source
}

function Write-Utf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText(
        $Path,
        $Content,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Write-AsciiFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Content, [System.Text.Encoding]::ASCII)
}

function Assert-CanonicalInputs {
    if ($Version -notmatch '^(?<major>0|[1-9][0-9]*)\.(?<minor>0|[1-9][0-9]*)\.(?<patch>0|[1-9][0-9]*)(?:-rc\.(?<rc>[1-9][0-9]*))?$') {
        throw 'Windows package builds accept canonical stable or rc.N release versions only'
    }
    if ($Tag -cne "v$Version") {
        throw 'Release tag must be the exact case-sensitive v-prefixed version'
    }
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
        throw 'Repository must use the owner/name form'
    }

    $versionPins = @(
        $ExpectedCMakeVersion,
        $ExpectedConanVersion,
        $ExpectedChocolateyVersion,
        $ExpectedPythonVersion
    )
    foreach ($versionPin in $versionPins) {
        if ($versionPin -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
            throw 'Release tool version pins must use the numeric major.minor.patch form'
        }
    }
    if ([version]$ExpectedCMakeVersion -lt [version]'3.25.0') {
        throw 'The pinned CMake version must be at least 3.25.0'
    }
    if ([version]$ExpectedConanVersion -lt [version]'2.0.0' -or
        [version]$ExpectedConanVersion -ge [version]'3.0.0') {
        throw 'The pinned Conan version must be from the Conan 2 series'
    }
    if ([version]$ExpectedChocolateyVersion -lt [version]'2.0.0' -or
        [version]$ExpectedChocolateyVersion -ge [version]'3.0.0') {
        throw 'The pinned Chocolatey version must be from the 2.x series'
    }
    $pythonPin = [version]$ExpectedPythonVersion
    if ($pythonPin.Major -ne 3 -or $pythonPin.Minor -ne 12) {
        throw 'The pinned Python version must be from the 3.12 series'
    }
    if ($ExpectedVCToolsVersion -notmatch '^14\.(?:3|4)[0-9]\.[0-9]+$') {
        throw 'The Visual C++ toolset pin must identify a VS 2022 v143 toolset'
    }
}

function Assert-ExactToolVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ToolPath,

        [Parameter()]
        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion,

        [Parameter(Mandatory = $true)]
        [string]$ToolName
    )

    $output = Invoke-CapturedCommand `
        -FilePath $ToolPath `
        -Arguments $Arguments `
        -FailureMessage "$ToolName version discovery failed"
    $versionMatches = [regex]::Matches($output, $Pattern)
    if ($versionMatches.Count -ne 1 -or
        $versionMatches[0].Groups[1].Value -cne $ExpectedVersion) {
        throw "$ToolName does not match its reviewed release version pin"
    }
}

function Read-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CacheText,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $match = [regex]::Match(
        $CacheText,
        "(?m)^$([regex]::Escape($Name))(?::[^=]+)?=(.*)$"
    )
    if (-not $match.Success) {
        throw "CMake cache is missing required release field: $Name"
    }
    return $match.Groups[1].Value.Trim()
}

function Assert-ReleaseProjectSettings {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,

        [Parameter(Mandatory = $true)]
        [ValidateSet('mcp-cpp-sdk-shared', 'mcp-cpp-sdk-static')]
        [string]$TargetName
    )

    $projects = @(Get-ChildItem -Path $BuildRoot -Recurse -Filter "$TargetName.vcxproj")
    if ($projects.Count -ne 1) {
        throw "Expected exactly one generated project for $TargetName"
    }

    [xml]$project = Get-Content -LiteralPath $projects[0].FullName -Raw
    $groups = @($project.SelectNodes("//*[local-name()='ItemDefinitionGroup']") | Where-Object {
        $_.Condition -match "Release\|x64"
    })
    if ($groups.Count -ne 1) {
        throw "$TargetName must have exactly one Release|x64 configuration"
    }

    $compile = $groups[0].SelectSingleNode("./*[local-name()='ClCompile']")
    if ($null -eq $compile) {
        throw "$TargetName has no Release|x64 compiler settings"
    }
    if ([string]$compile.RuntimeLibrary -cne 'MultiThreadedDLL') {
        throw "$TargetName is not using the dynamic release MSVC runtime"
    }
    if ([string]$compile.Optimization -cne 'MaxSpeed') {
        throw "$TargetName is not using release optimization"
    }
    if ([string]$compile.PreprocessorDefinitions -notmatch '(?:^|;)NDEBUG(?:;|$)') {
        throw "$TargetName is missing NDEBUG in Release|x64"
    }
}

function Assert-InstalledConsumer {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('shared', 'static')]
        [string]$Linkage,

        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,

        [Parameter(Mandatory = $true)]
        [string]$StageRoot,

        [Parameter(Mandatory = $true)]
        [string]$ToolchainFile,

        [Parameter(Mandatory = $true)]
        [string]$CMakePath
    )

    $consumerSource = Join-Path $BuildRoot "consumer-$Linkage-src"
    $consumerBuild = Join-Path $BuildRoot "consumer-$Linkage-build"
    $target = "mcp::sdk_$Linkage"
    $executableName = "mcp-cpp-sdk-$Linkage-consumer"

    $cmakeLists = @(
        'cmake_minimum_required(VERSION 3.25)',
        "project($executableName LANGUAGES CXX)",
        'find_package(mcp-cpp-sdk CONFIG REQUIRED)',
        "add_executable($executableName main.cpp)",
        "target_link_libraries($executableName PRIVATE $target)",
        "target_compile_features($executableName PRIVATE cxx_std_20)",
        ''
    ) -join "`n"
    $mainSource = @(
        '#include <mcp/core/version.hpp>',
        '',
        'int main() {',
        '    return mcp::version().empty() ? 1 : 0;',
        '}',
        ''
    ) -join "`n"
    Write-Utf8File -Path (Join-Path $consumerSource 'CMakeLists.txt') -Content $cmakeLists
    Write-Utf8File -Path (Join-Path $consumerSource 'main.cpp') -Content $mainSource

    Invoke-CheckedCommand -FilePath $CMakePath -Arguments @(
        '-S', $consumerSource,
        '-B', $consumerBuild,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        '-T', 'v143',
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
        "-DCMAKE_PREFIX_PATH=$StageRoot",
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        '-DBUILD_TESTING=OFF'
    ) -FailureMessage "Installed $Linkage consumer configuration failed"
    Invoke-CheckedCommand -FilePath $CMakePath -Arguments @(
        '--build', $consumerBuild,
        '--config', 'Release',
        '--parallel'
    ) -FailureMessage "Installed $Linkage consumer build failed"

    $executable = Join-Path $consumerBuild "Release\$executableName.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Installed $Linkage consumer executable is missing"
    }

    $originalPath = $env:PATH
    try {
        $env:PATH = "$(Join-Path $StageRoot 'bin');$originalPath"
        Invoke-CheckedCommand `
            -FilePath $executable `
            -FailureMessage "Installed $Linkage consumer execution failed"
    }
    finally {
        $env:PATH = $originalPath
    }
}

function Assert-ExactOutputInventory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$OutputRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedNames
    )

    $actual = @(Get-ChildItem -LiteralPath $OutputRoot -File | ForEach-Object Name | Sort-Object)
    $expected = @($ExpectedNames | Sort-Object)
    $difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
    if ($difference.Count -ne 0) {
        throw 'Windows release output inventory differs from the reviewed contract'
    }
}

function Start-LoopbackArchiveServer {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PythonPath,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,

        [Parameter(Mandatory = $true)]
        [string]$UrlFile
    )

    if (Test-Path -LiteralPath $UrlFile) {
        throw 'Loopback URL handoff file must be new'
    }
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $PythonPath
    $startInfo.WorkingDirectory = $RepositoryRoot
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in @(
            '-I', '-S', 'scripts/run_release_tool.py',
            'release.loopback_archive',
            '--archive', $ArchivePath,
            '--url-file', $UrlFile
        )) {
        $startInfo.ArgumentList.Add($argument)
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'Loopback archive server did not start'
    }

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if (Test-Path -LiteralPath $UrlFile -PathType Leaf) {
            return $process
        }
        if ($process.HasExited) {
            $errorText = $process.StandardError.ReadToEnd().Trim()
            throw "Loopback archive server exited before readiness: $errorText"
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $process.HasExited) {
        $process.Kill($true)
        $process.WaitForExit()
    }
    throw 'Loopback archive server did not become ready'
}

function Stop-LoopbackArchiveServer {
    param(
        [Parameter()]
        [System.Diagnostics.Process]$Process
    )

    if ($null -eq $Process) {
        return
    }
    try {
        if (-not $Process.HasExited) {
            $Process.Kill($true)
            $Process.WaitForExit()
        }
    }
    finally {
        $Process.Dispose()
    }
}

function Invoke-ChocolateyCandidateValidation {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ChocolateyPath,

        [Parameter(Mandatory = $true)]
        [string]$PythonPath,

        [Parameter(Mandatory = $true)]
        [string]$CMakePath,

        [Parameter(Mandatory = $true)]
        [string]$RepositoryRoot,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,

        [Parameter(Mandatory = $true)]
        [string]$ToolchainFile,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedTree
    )

    $existingRoot = [Environment]::GetEnvironmentVariable(
        'MCP_CPP_SDK_ROOT',
        [EnvironmentVariableTarget]::Machine
    )
    if ($existingRoot) {
        throw 'Chocolatey validation requires no pre-existing MCP_CPP_SDK_ROOT'
    }

    $validationRoot = Join-Path $RepositoryRoot 'build/release/chocolatey-validation'
    $candidateRoot = Join-Path $validationRoot 'package'
    $packageSource = Join-Path $validationRoot 'source'
    $urlFile = Join-Path $validationRoot 'archive-url.txt'
    $loopbackProcess = $null
    $installAttempted = $false
    New-Item -ItemType Directory -Path $candidateRoot, $packageSource -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'chocolatey') -Force |
        Copy-Item -Destination $candidateRoot -Recurse -Force

    try {
        $loopbackProcess = Start-LoopbackArchiveServer `
            -PythonPath $PythonPath `
            -RepositoryRoot $RepositoryRoot `
            -ArchivePath $ArchivePath `
            -UrlFile $urlFile
        $localUrl = [System.IO.File]::ReadAllText($urlFile).Trim()
        if ($localUrl -notmatch '^http://127\.0\.0\.1:[1-9][0-9]*/[^/?#]+$') {
            throw 'Loopback archive URL is not exact'
        }

        Invoke-CheckedCommand -FilePath $PythonPath -Arguments @(
            '-I', '-S', 'scripts/run_release_tool.py',
            'release.package_validation', 'chocolatey-adapt',
            '--metadata', (Join-Path $RepositoryRoot 'chocolatey/tools/chocolateyinstall.ps1'),
            '--archive', $ArchivePath,
            '--local-url', $localUrl,
            '--output', (Join-Path $candidateRoot 'tools/chocolateyinstall.ps1')
        ) -FailureMessage 'Chocolatey loopback-only candidate adaptation failed'
        Invoke-CheckedCommand -FilePath $ChocolateyPath -Arguments @(
            'pack', (Join-Path $candidateRoot 'mcp-cpp-sdk.nuspec'),
            '--outputdirectory', $packageSource,
            '--yes'
        ) -FailureMessage 'Chocolatey functional candidate creation failed'
        $candidatePackages = @(Get-ChildItem -LiteralPath $packageSource -File -Filter '*.nupkg')
        if ($candidatePackages.Count -ne 1 -or
            $candidatePackages[0].Name -cne "mcp-cpp-sdk.$Version.nupkg") {
            throw 'Chocolatey functional candidate inventory is not exact'
        }

        $installAttempted = $true
        Invoke-CheckedCommand -FilePath $ChocolateyPath -Arguments @(
            'install', 'mcp-cpp-sdk',
            '--version', $Version,
            '--source', $packageSource,
            '--ignore-dependencies',
            '--yes',
            '--no-progress'
        ) -FailureMessage 'Chocolatey candidate installation failed'
        $installedRoot = [Environment]::GetEnvironmentVariable(
            'MCP_CPP_SDK_ROOT',
            [EnvironmentVariableTarget]::Machine
        )
        if (-not $installedRoot -or
            [System.IO.Path]::GetFileName($installedRoot) -cne 'mcp-cpp-sdk' -or
            -not (Test-Path -LiteralPath $installedRoot -PathType Container)) {
            throw 'Chocolatey candidate did not install its exact machine environment'
        }
        Invoke-CheckedCommand -FilePath $PythonPath -Arguments @(
            '-I', '-S', 'scripts/run_release_tool.py',
            'release.package_validation', 'compare-trees',
            '--expected', $ExpectedTree,
            '--actual', $installedRoot
        ) -FailureMessage 'Chocolatey installed tree differs from the candidate archive'
        Assert-InstalledConsumer `
            -Linkage 'shared' `
            -BuildRoot (Join-Path $validationRoot 'consumer') `
            -StageRoot $installedRoot `
            -ToolchainFile $ToolchainFile `
            -CMakePath $CMakePath
        Assert-InstalledConsumer `
            -Linkage 'static' `
            -BuildRoot (Join-Path $validationRoot 'consumer') `
            -StageRoot $installedRoot `
            -ToolchainFile $ToolchainFile `
            -CMakePath $CMakePath

        Invoke-CheckedCommand -FilePath $ChocolateyPath -Arguments @(
            'uninstall', 'mcp-cpp-sdk',
            '--version', $Version,
            '--yes',
            '--no-progress'
        ) -FailureMessage 'Chocolatey candidate uninstallation failed'
        $installAttempted = $false
        if ([Environment]::GetEnvironmentVariable(
                'MCP_CPP_SDK_ROOT',
                [EnvironmentVariableTarget]::Machine
            )) {
            throw 'Chocolatey candidate left MCP_CPP_SDK_ROOT after uninstall'
        }
        if ((Test-Path -LiteralPath $installedRoot) -or
            (Test-Path -LiteralPath "$installedRoot.installing")) {
            throw 'Chocolatey candidate left its installation tree after uninstall'
        }
    }
    finally {
        if ($installAttempted) {
            & $ChocolateyPath uninstall mcp-cpp-sdk --version $Version `
                --yes --no-progress 2>&1 | Out-Null
        }
        Stop-LoopbackArchiveServer -Process $loopbackProcess
    }
}

Assert-CanonicalInputs

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$originalLocation = Get-Location
$probeSource = $null
$probeObject = $null
$probeExecutable = $null
$factsPath = $null

try {
    Set-Location -LiteralPath $repoRoot
    $versionMatch = [regex]::Match(
        $Version,
        '^(?<major>0|[1-9][0-9]*)\.(?<minor>0|[1-9][0-9]*)\.(?<patch>0|[1-9][0-9]*)(?:-rc\.(?<rc>[1-9][0-9]*))?$'
    )
    if (-not $versionMatch.Success) {
        throw 'Release version stopped matching after canonical input validation'
    }
    $versionCore = "$($versionMatch.Groups['major'].Value).$($versionMatch.Groups['minor'].Value).$($versionMatch.Groups['patch'].Value)"
    $sourceVersion = (Get-Content -LiteralPath 'VERSION' -Raw).Trim()
    if ($sourceVersion -cne $Version) {
        throw 'Requested release version does not match the checked-out source version'
    }
    $abiVersion = if ($versionMatch.Groups['major'].Value -eq '0') {
        $versionCore
    }
    else {
        $versionMatch.Groups['major'].Value
    }

    Remove-Item -Path @(
        'build/release',
        'build/consumer-shared-src',
        'build/consumer-shared-build',
        'build/consumer-static-src',
        'build/consumer-static-build',
        'stage/release',
        'out',
        'chocolatey'
    ) -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -Path @(
        'build/release',
        'stage/release',
        'out',
        'chocolatey/tools'
    ) -ItemType Directory -Force | Out-Null

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio installation discovery is unavailable'
    }
    $installationText = Invoke-CapturedCommand -FilePath $vswhere -Arguments @(
        '-products', '*',
        '-version', '[17.0,18.0)',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-property', 'installationPath'
    ) -FailureMessage 'Visual Studio 2022 C++ installation discovery failed'
    $installations = @($installationText -split "`r?`n" | Where-Object { $_.Trim() })
    if ($installations.Count -ne 1) {
        throw 'Expected exactly one Visual Studio 2022 C++ installation'
    }
    $visualStudioRoot = [System.IO.Path]::GetFullPath($installations[0].Trim())

    if ($env:VisualStudioVersion -notmatch '^17\.') {
        throw 'The release shell is not initialized for Visual Studio 2022'
    }
    if ($env:VSCMD_ARG_TGT_ARCH -cne 'x64' -or $env:VSCMD_ARG_HOST_ARCH -cne 'x64') {
        throw 'The release shell must use the native x64 MSVC host and target'
    }
    if (-not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
        throw 'Windows release packaging requires a 64-bit process on a 64-bit OS'
    }
    if ($env:PROCESSOR_ARCHITECTURE -cne 'AMD64') {
        throw 'Windows release packaging requires the AMD64 process architecture'
    }
    if (-not $env:VCToolsVersion) {
        throw 'The active Visual C++ tools version is unavailable'
    }
    $actualVCToolsVersion = $env:VCToolsVersion.Trim().TrimEnd('\')
    if ($actualVCToolsVersion -cne $ExpectedVCToolsVersion) {
        throw 'The active Visual C++ tools do not match the reviewed release pin'
    }

    $cl = Get-RequiredApplication -Name 'cl.exe'
    $cmake = Get-RequiredApplication -Name 'cmake.exe'
    $ctest = Get-RequiredApplication -Name 'ctest.exe'
    $conan = Get-RequiredApplication -Name 'conan.exe'
    $dumpbin = Get-RequiredApplication -Name 'dumpbin.exe'
    $choco = Get-RequiredApplication -Name 'choco.exe'
    $python = Get-RequiredApplication -Name 'python.exe'

    $expectedCompilerRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $visualStudioRoot "VC\Tools\MSVC\$ExpectedVCToolsVersion")
    )
    $compilerPath = [System.IO.Path]::GetFullPath($cl)
    if (-not $compilerPath.StartsWith(
            "$expectedCompilerRoot\",
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or $compilerPath -notmatch '\\bin\\Hostx64\\x64\\cl\.exe$') {
        throw 'The active compiler is not the pinned native x64 VS 2022 compiler'
    }
    $dumpbinPath = [System.IO.Path]::GetFullPath($dumpbin)
    if (-not $dumpbinPath.StartsWith(
            "$expectedCompilerRoot\",
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or $dumpbinPath -notmatch '\\bin\\Hostx64\\x64\\dumpbin\.exe$') {
        throw 'The active binary inspector is not from the pinned native x64 toolset'
    }
    $cmakeDirectory = [System.IO.Path]::GetFullPath((Split-Path -Parent $cmake))
    $ctestDirectory = [System.IO.Path]::GetFullPath((Split-Path -Parent $ctest))
    if (-not $cmakeDirectory.Equals(
            $ctestDirectory,
            [System.StringComparison]::OrdinalIgnoreCase
        )) {
        throw 'CMake and CTest must come from the same reviewed installation'
    }

    Assert-ExactToolVersion `
        -ToolPath $cmake `
        -Arguments @('--version') `
        -Pattern '(?m)^cmake version ([0-9]+\.[0-9]+\.[0-9]+)\r?$' `
        -ExpectedVersion $ExpectedCMakeVersion `
        -ToolName 'CMake'
    Assert-ExactToolVersion `
        -ToolPath $conan `
        -Arguments @('--version') `
        -Pattern '(?m)^Conan version ([0-9]+\.[0-9]+\.[0-9]+)$' `
        -ExpectedVersion $ExpectedConanVersion `
        -ToolName 'Conan'
    Assert-ExactToolVersion `
        -ToolPath $choco `
        -Arguments @('--version') `
        -Pattern '^([0-9]+\.[0-9]+\.[0-9]+)$' `
        -ExpectedVersion $ExpectedChocolateyVersion `
        -ToolName 'Chocolatey'
    Assert-ExactToolVersion `
        -ToolPath $python `
        -Arguments @('--version') `
        -Pattern '^Python ([0-9]+\.[0-9]+\.[0-9]+)$' `
        -ExpectedVersion $ExpectedPythonVersion `
        -ToolName 'Python'

    $probeSource = Join-Path $repoRoot 'build/release/msvc_probe.cpp'
    $probeObject = Join-Path $repoRoot 'build/release/msvc_probe.obj'
    $probeExecutable = Join-Path $repoRoot 'build/release/msvc_probe.exe'
    Write-AsciiFile -Path $probeSource -Content @'
#include <iostream>
int main() { std::cout << _MSC_VER << " " << sizeof(void*) << "\n"; }
'@
    Invoke-CapturedCommand -FilePath $cl -Arguments @(
        '/nologo', '/EHsc', '/O2', '/MD', '/DNDEBUG', $probeSource,
        "/Fo$probeObject", "/Fe$probeExecutable"
    ) -FailureMessage 'MSVC compiler identity probe compilation failed' | Out-Null
    $probeOutput = Invoke-CapturedCommand `
        -FilePath $probeExecutable `
        -FailureMessage 'MSVC compiler identity probe execution failed'
    $probeMatch = [regex]::Match($probeOutput, '^([0-9]+) 8$')
    if (-not $probeMatch.Success) {
        throw 'MSVC compiler identity probe produced an unexpected result'
    }
    $mscVer = [int]$probeMatch.Groups[1].Value
    if ($mscVer -ne $ExpectedMscVer) {
        throw 'The MSVC compiler identity does not match the reviewed release pin'
    }

    $compilerVersion = if ($mscVer -ge 1940) { '194' } else { '193' }
    $profilePath = Join-Path $repoRoot 'release/windows/conan-msvc-release.profile'
    $profileText = [System.IO.File]::ReadAllText($profilePath)
    if ($profileText -notmatch "(?m)^compiler\.version=$([regex]::Escape($compilerVersion))$") {
        throw 'The reviewed Conan lock profile does not match the active MSVC series'
    }

    Invoke-CheckedCommand -FilePath $conan -Arguments @(
        'install', '.',
        '--output-folder=build/release',
        '--build=missing',
        '--lockfile=release/windows/conan.lock',
        "--profile:host=$profilePath",
        "--profile:build=$profilePath"
    ) -FailureMessage 'Conan dependency installation failed'
    $toolchainFile = (Resolve-Path -LiteralPath `
        'build/release/build/generators/conan_toolchain.cmake').Path

    Invoke-CheckedCommand -FilePath $cmake -Arguments @(
        '-S', '.',
        '-B', 'build/release',
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        '-T', 'v143',
        "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile",
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        '-DMCP_CPP_SDK_BUILD_SHARED=ON',
        '-DMCP_CPP_SDK_BUILD_STATIC=ON',
        '-DMCP_CPP_SDK_DEFAULT_LINKAGE=shared',
        '-DBUILD_TESTING=ON',
        '-DBUILD_EXAMPLES=OFF',
        '-DBUILD_DOCS=OFF'
    ) -FailureMessage 'Visual Studio 2022 release configuration failed'
    Invoke-CheckedCommand -FilePath $cmake -Arguments @(
        '--build', 'build/release',
        '--config', 'Release',
        '--parallel'
    ) -FailureMessage 'Visual Studio 2022 release build failed'
    Invoke-CheckedCommand -FilePath $ctest -Arguments @(
        '--test-dir', 'build/release',
        '-C', 'Release',
        '--output-on-failure'
    ) -FailureMessage 'Visual Studio 2022 release tests failed'
    Invoke-CheckedCommand -FilePath $cmake -Arguments @(
        '--install', 'build/release',
        '--config', 'Release',
        '--prefix', 'stage/release'
    ) -FailureMessage 'Visual Studio 2022 release installation failed'

    $cacheText = Get-Content -LiteralPath 'build/release/CMakeCache.txt' -Raw
    $cacheFacts = [ordered]@{
        CMAKE_GENERATOR = Read-CMakeCacheValue `
            -CacheText $cacheText -Name 'CMAKE_GENERATOR'
        CMAKE_GENERATOR_PLATFORM = Read-CMakeCacheValue `
            -CacheText $cacheText -Name 'CMAKE_GENERATOR_PLATFORM'
        CMAKE_GENERATOR_TOOLSET = Read-CMakeCacheValue `
            -CacheText $cacheText -Name 'CMAKE_GENERATOR_TOOLSET'
        CMAKE_MSVC_RUNTIME_LIBRARY = Read-CMakeCacheValue `
            -CacheText $cacheText -Name 'CMAKE_MSVC_RUNTIME_LIBRARY'
    }
    $requiredCacheValues = [ordered]@{
        MCP_CPP_SDK_BUILD_SHARED = 'ON'
        MCP_CPP_SDK_BUILD_STATIC = 'ON'
        MCP_CPP_SDK_DEFAULT_LINKAGE = 'shared'
        BUILD_TESTING = 'ON'
        BUILD_EXAMPLES = 'OFF'
        BUILD_DOCS = 'OFF'
    }
    foreach ($cacheEntry in $requiredCacheValues.GetEnumerator()) {
        $actualValue = Read-CMakeCacheValue -CacheText $cacheText -Name $cacheEntry.Key
        if ($actualValue -cne $cacheEntry.Value) {
            throw "CMake cache does not prove the required value for $($cacheEntry.Key)"
        }
    }
    Assert-ReleaseProjectSettings `
        -BuildRoot (Join-Path $repoRoot 'build/release') `
        -TargetName 'mcp-cpp-sdk-shared'
    Assert-ReleaseProjectSettings `
        -BuildRoot (Join-Path $repoRoot 'build/release') `
        -TargetName 'mcp-cpp-sdk-static'

    $compilerFiles = @(Get-ChildItem -Path 'build/release/CMakeFiles' -Recurse `
        -Filter 'CMakeCXXCompiler.cmake')
    if ($compilerFiles.Count -lt 1) {
        throw 'CMake compiler identity metadata is missing'
    }
    foreach ($compilerFile in $compilerFiles) {
        $compilerMetadata = Get-Content -LiteralPath $compilerFile.FullName -Raw
        if ($compilerMetadata -notmatch 'set\(CMAKE_CXX_COMPILER_ID "MSVC"\)' -or
            $compilerMetadata -notmatch 'set\(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "?x64"?\)') {
            throw 'CMake compiler identity metadata is not MSVC x64'
        }
    }

    $stageRoot = (Resolve-Path -LiteralPath 'stage/release').Path
    Assert-InstalledConsumer `
        -Linkage 'shared' `
        -BuildRoot (Join-Path $repoRoot 'build') `
        -StageRoot $stageRoot `
        -ToolchainFile $toolchainFile `
        -CMakePath $cmake
    Assert-InstalledConsumer `
        -Linkage 'static' `
        -BuildRoot (Join-Path $repoRoot 'build') `
        -StageRoot $stageRoot `
        -ToolchainFile $toolchainFile `
        -CMakePath $cmake

    $sdkDlls = @(Get-ChildItem -LiteralPath 'stage/release/bin' `
        -Filter 'mcp-cpp-sdk*.dll' -File)
    if ($sdkDlls.Count -ne 1) {
        throw 'Expected exactly one installed MCP C++ SDK DLL'
    }
    $dllHeaders = Invoke-CapturedCommand -FilePath $dumpbin -Arguments @(
        '/HEADERS', $sdkDlls[0].FullName
    ) -FailureMessage 'Installed SDK DLL architecture inspection failed'
    if ($dllHeaders -notmatch '(?im)\b8664 machine \(x64\)') {
        throw 'Installed SDK DLL is not an x64 PE image'
    }
    $dependents = Invoke-CapturedCommand -FilePath $dumpbin -Arguments @(
        '/DEPENDENTS', $sdkDlls[0].FullName
    ) -FailureMessage 'Installed SDK DLL runtime inspection failed'

    $sdkStaticLibraries = @(Get-ChildItem -LiteralPath 'stage/release/lib' `
        -Filter 'mcp-cpp-sdk-static*.lib' -File)
    if ($sdkStaticLibraries.Count -ne 1) {
        throw 'Expected exactly one installed MCP C++ SDK static library'
    }
    $staticDirectives = Invoke-CapturedCommand -FilePath $dumpbin -Arguments @(
        '/DIRECTIVES', $sdkStaticLibraries[0].FullName
    ) -FailureMessage 'Installed SDK static library runtime inspection failed'
    if ($staticDirectives -notmatch '(?i)/DEFAULTLIB:MSVCRT\b' -or
        $staticDirectives -match '(?i)\b(?:LIBCMTD?|MSVCRTD)\b') {
        throw 'Installed SDK static library does not use the dynamic release CRT'
    }

    $dependentEvidence = @{}
    $dependentEvidence[$sdkDlls[0].Name] = $dependents
    $factsPath = Join-Path $repoRoot 'build/release/windows-build-facts.json'
    $facts = [ordered]@{
        os_architecture = '64-bit'
        process_architecture = 'AMD64'
        compiler_id = 'MSVC'
        msc_ver = $mscVer
        pointer_bits = 64
        build_configuration = 'Release'
        cache = $cacheFacts
        shared_compile_flags = @('/MD', '/O2', '/DNDEBUG')
        dumpbin_dependents = $dependentEvidence
    }
    Write-Utf8File `
        -Path $factsPath `
        -Content ($facts | ConvertTo-Json -Depth 5 -Compress)

    $identityName = 'build-identity-windows-x64-v143-md.json'
    $identityPath = Join-Path $repoRoot "out/$identityName"
    Invoke-CheckedCommand -FilePath $python -Arguments @(
        '-I', '-S', 'scripts/release/collect_build_identity.py',
        '--kind', 'windows', '--facts', $factsPath,
        '--abi-version', $abiVersion, '--output', $identityPath
    ) -FailureMessage 'Windows build identity validation failed'

    $archiveName = "mcp-cpp-sdk-$Version-windows-x64-v143-md.zip"
    $archivePath = Join-Path $repoRoot "out/$archiveName"
    Compress-Archive -Path 'stage/release/*' -DestinationPath $archivePath
    $sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()

    $archiveUrl = "https://github.com/$Repository/releases/download/$Tag/$archiveName"
    $installValuesPath = Join-Path $repoRoot 'build/release/chocolatey-install-values.json'
    $nuspecValuesPath = Join-Path $repoRoot 'build/release/chocolatey-nuspec-values.json'
    $installValues = [ordered]@{
        VERSION = $Version
        ARCHIVE_URL = $archiveUrl
        ARCHIVE_SHA256 = $sha256
    }
    $nuspecValues = [ordered]@{
        VERSION = $Version
        AUTHORS = 'mcp-cpp-sdk contributors'
        OWNERS = 'mcp-cpp-sdk maintainers'
        SUMMARY = 'Model Context Protocol C++ SDK'
        DESCRIPTION = 'C++20 Model Context Protocol SDK development package.'
        PROJECT_URL = "https://github.com/$Repository"
        PACKAGE_SOURCE_URL = "https://github.com/$Repository"
        COPYRIGHT = 'mcp-cpp-sdk contributors'
        TAGS = 'mcp cpp sdk development admin'
    }
    Write-Utf8File -Path $installValuesPath -Content (
        $installValues | ConvertTo-Json -Compress
    )
    Write-Utf8File -Path $nuspecValuesPath -Content (
        $nuspecValues | ConvertTo-Json -Compress
    )
    Invoke-CheckedCommand -FilePath $python -Arguments @(
        '-m', 'release.cli', 'render',
        '--template', 'packaging/chocolatey/chocolateyinstall.ps1.in',
        '--values', $installValuesPath,
        '--output', 'chocolatey/tools/chocolateyinstall.ps1'
    ) -FailureMessage 'Chocolatey metadata rendering failed'
    Invoke-CheckedCommand -FilePath $python -Arguments @(
        '-m', 'release.cli', 'render',
        '--template', 'packaging/chocolatey/mcp-cpp-sdk.nuspec.in',
        '--values', $nuspecValuesPath,
        '--output', 'chocolatey/mcp-cpp-sdk.nuspec'
    ) -FailureMessage 'Chocolatey package metadata rendering failed'
    Copy-Item -LiteralPath 'packaging/chocolatey/chocolateyuninstall.ps1.in' `
        -Destination 'chocolatey/tools/chocolateyuninstall.ps1'
    Copy-Item -LiteralPath 'LICENSE' -Destination 'chocolatey/LICENSE.txt'
    Write-Utf8File -Path 'chocolatey/VERIFICATION.txt' -Content (
        "The archive SHA-256 is $sha256 and is bound by the immutable GitHub Release manifest.`n"
    )
    Invoke-CheckedCommand -FilePath $choco -Arguments @(
        'pack', 'chocolatey/mcp-cpp-sdk.nuspec',
        '--outputdirectory', 'out',
        '--yes'
    ) -FailureMessage 'Chocolatey package creation failed'

    Invoke-ChocolateyCandidateValidation `
        -ChocolateyPath $choco `
        -PythonPath $python `
        -CMakePath $cmake `
        -RepositoryRoot $repoRoot `
        -Version $Version `
        -ArchivePath $archivePath `
        -ToolchainFile $toolchainFile `
        -ExpectedTree $stageRoot

    $nupkgName = "mcp-cpp-sdk.$Version.nupkg"
    Assert-ExactOutputInventory -OutputRoot (Join-Path $repoRoot 'out') -ExpectedNames @(
        $identityName,
        $archiveName,
        $nupkgName
    )
}
finally {
    foreach ($temporaryFile in @($probeSource, $probeObject, $probeExecutable, $factsPath)) {
        if ($temporaryFile -and (Test-Path -LiteralPath $temporaryFile)) {
            Remove-Item -Force -LiteralPath $temporaryFile
        }
    }
    Set-Location -LiteralPath $originalLocation.Path
}
