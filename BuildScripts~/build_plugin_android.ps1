param(
    [ValidateSet("release", "debug")]
    [string]$BuildType = "release"
)

$ErrorActionPreference = "Stop"

function Convert-ToGitBashPath {
    param([Parameter(Mandatory = $true)][string]$WindowsPath)
    $normalized = $WindowsPath.Replace("\", "/")
    if ($normalized -match "^[A-Za-z]:/") {
        $drive = $normalized.Substring(0, 1).ToLowerInvariant()
        $rest = $normalized.Substring(2)
        return "/$drive$rest"
    }
    return $normalized
}

function Resolve-UnityNdkPath {
    $envCandidates = @(
        $env:ANDROID_NDK,
        $env:ANDROID_NDK_ROOT,
        $env:ANDROID_NDK_HOME
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    foreach ($candidate in $envCandidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $unityEditorsRoot = "C:/Program Files/Unity/Hub/Editor"
    if (-not (Test-Path $unityEditorsRoot)) {
        return $null
    }

    $ndkDirs = Get-ChildItem $unityEditorsRoot -Directory |
        Sort-Object Name |
        ForEach-Object {
            Join-Path $_.FullName "Editor/Data/PlaybackEngines/AndroidPlayer/NDK"
        } |
        Where-Object { Test-Path $_ }

    if ($ndkDirs.Count -gt 0) {
        return $ndkDirs[-1]
    }

    return $null
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot
$bashScriptPath = Join-Path $scriptRoot "build_plugin_android.sh"

$gitBashExe = "C:/Program Files/Git/bin/bash.exe"
if (-not (Test-Path $gitBashExe)) {
    throw "Git Bash not found at '$gitBashExe'. Install Git for Windows or update this script path."
}

if (-not (Test-Path $bashScriptPath)) {
    throw "Bash build script not found at '$bashScriptPath'."
}

$ndkPath = Resolve-UnityNdkPath
if (-not $ndkPath) {
    throw "Android NDK not found. Set ANDROID_NDK/ANDROID_NDK_ROOT/ANDROID_NDK_HOME, or install Android support in Unity Hub."
}

$repoRootBash = Convert-ToGitBashPath -WindowsPath $repoRoot
$ndkPathBash = Convert-ToGitBashPath -WindowsPath $ndkPath

Write-Host "Using Android NDK: $ndkPath"
Write-Host "Running build: $BuildType"

# Pass NDK path via process env (NOT inline `ANDROID_NDK=... cmd` prefix in the
# bash command string). PowerShell's `& bash.exe -c '...'` native-argument
# parsing silently breaks when the command string contains embedded double
# quotes -- the bash process receives a mangled arg, exits 0, prints nothing,
# and we falsely report success (observed 2026-05-15, "completed successfully"
# with empty output and a stale aar).
$env:ANDROID_NDK = $ndkPathBash

# Use -c (NOT -lc) so we don't source the user's .bashrc/.profile. Anaconda
# init blocks inside the user profile can also exit silently before the script
# runs.
$bashCommand = "cd '$repoRootBash' && ./BuildScripts~/build_plugin_android.sh $BuildType"
& $gitBashExe -c $bashCommand

if ($LASTEXITCODE -ne 0) {
    throw "Android plugin build failed with exit code $LASTEXITCODE."
}

Write-Host "Android plugin build completed successfully."
