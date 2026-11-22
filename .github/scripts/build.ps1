$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer discovery tool was not found.'
}

$installation = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'A Visual Studio installation with the x64 C++ toolchain was not found.'
}

$devShellModule = Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShellModule
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64'

Push-Location (Split-Path (Split-Path $PSScriptRoot))
try {
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUE4SSLEB_BUILD_TESTS=OFF
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    cmake --build build --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally { Pop-Location }
