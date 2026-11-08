# Build the minimal static FFmpeg used by ImView.
#
# Windows entry point: loads the MSVC x64 environment, finds a POSIX shell,
# exposes the applets of a standalone busybox shell, and runs
# tools/build_ffmpeg.sh. Pass an optional build directory as the first argument;
# the default is build\ffmpeg-build.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$shScript = Join-Path $PSScriptRoot 'build_ffmpeg.sh'

# The pinned GNU make and NASM builds live in the Git-ignored 3rd\tools.
# The download helper verifies existing files and only uses the network when
# one is missing or does not match its checksum.
& (Join-Path $PSScriptRoot 'download_ffmpeg_tools.ps1')

if (-not $env:VCINSTALLDIR) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($vsPath) {
            $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $vcvars) {
                $envLines = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
                foreach ($line in $envLines) {
                    $separator = $line.IndexOf('=')
                    if ($separator -gt 0) {
                        [Environment]::SetEnvironmentVariable(
                            $line.Substring(0, $separator),
                            $line.Substring($separator + 1),
                            'Process')
                    }
                }
            }
        }
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe was not found. Install the Visual Studio C++ build tools, or run this from an x64 developer prompt.'
}

# Prefer a shell already on PATH, then the usual Git for Windows and MSYS2
# locations. A standalone busybox shell is also accepted.
$shellCandidates = @()
$pathShell = Get-Command sh.exe -ErrorAction SilentlyContinue
if ($pathShell) { $shellCandidates += $pathShell.Source }
$shellCandidates += @(
    (Join-Path $env:ProgramFiles 'Git\usr\bin\sh.exe'),
    (Join-Path $env:ProgramFiles 'Git\bin\bash.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Git\usr\bin\sh.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Git\bin\bash.exe'),
    'C:\msys64\usr\bin\sh.exe',
    'C:\msys64\usr\bin\bash.exe',
    (Join-Path $env:USERPROFILE '.local\bin\sh.exe')
)
$shell = $shellCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $shell) {
    throw 'No POSIX shell was found. Install Git for Windows or MSYS2, or put sh.exe on PATH.'
}
$env:PATH = "$(Split-Path -Parent $shell);$env:PATH"

$busybox = $false
try {
    & $shell -c 'busybox --list' *> $null
    $busybox = ($LASTEXITCODE -eq 0)
} catch {
    $busybox = $false
}
if ($busybox) {
    $tools = Join-Path $root 'build\ffmpeg-tools'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    $sourceShell = (Resolve-Path $shell).Path
    $shellCopy = Join-Path $tools 'sh.exe'
    if (-not (Test-Path $shellCopy)) {
        Copy-Item $sourceShell $shellCopy
    }
    $applets = @(
        'bash', 'sed', 'awk', 'grep', 'egrep', 'fgrep', 'cat', 'cmp', 'sort',
        'rm', 'cp', 'mkdir', 'mv', 'printf', 'tr', 'head', 'tail', 'wc', 'cut',
        'find', 'expr', 'date', 'env', 'basename', 'dirname', 'uname', 'test',
        'ls', 'ln', 'touch', 'patch', 'diff', 'od', 'xxd', 'tar', 'gzip',
        'which', 'chmod', 'true', 'false', 'sleep', 'tee', 'xargs', 'install'
    )
    foreach ($applet in $applets) {
        $target = Join-Path $tools "$applet.exe"
        if (Test-Path $target) { continue }
        try {
            New-Item -ItemType HardLink -Path $target -Target $sourceShell | Out-Null
        } catch {
            Copy-Item $sourceShell $target
        }
    }
    $shell = $shellCopy
    $env:PATH = "$tools;$env:PATH"
}

$env:MAKESHELL = $shell
& $shell $shScript @args
exit $LASTEXITCODE
