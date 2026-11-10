param(
    [string]$PythonExecutable,
    [string]$Qemu,
    [string]$Workspace
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $PythonExecutable) {
    foreach ($candidate in @(
        (Join-Path $projectRoot 'python\pythonw.exe'),
        (Join-Path $projectRoot 'runtime\python\pythonw.exe'),
        (Join-Path $projectRoot 'tools\python\pythonw.exe'),
        (Join-Path $projectRoot 'dist\windows\python\pythonw.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $PythonExecutable = $candidate
            break
        }
    }
}
if (-not $PythonExecutable) {
    $python = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($python -and $python.Source -notlike '*\WindowsApps\*') {
        $PythonExecutable = $python.Source
    }
}
if (-not $PythonExecutable) {
    throw 'Python with Tk and Pillow is required. Supply -PythonExecutable or use the packaged Windows runtime.'
}
if (-not $Workspace) { $Workspace = Join-Path $env:LOCALAPPDATA 'AzkiSimulator\Windows' }
$portableApp = Join-Path $projectRoot 'windows-app\app.py'
if (Test-Path -LiteralPath $portableApp -PathType Leaf) {
    $app = $portableApp
    $engineProject = Join-Path $projectRoot 'project'
    if (-not $Qemu) { $Qemu = Join-Path $projectRoot 'qemu-system-arm.exe' }
} else {
    $app = Join-Path $projectRoot 'engine\windows-app\app.py'
    $engineProject = Join-Path $projectRoot 'engine\project'
    if (-not $Qemu) { $Qemu = Join-Path $projectRoot 'dist\windows\qemu-system-arm.exe' }
}
& $PythonExecutable $app --project-root $engineProject --qemu $Qemu --workspace $Workspace
if ($LASTEXITCODE) { exit $LASTEXITCODE }
