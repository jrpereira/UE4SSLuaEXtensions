# Run in an x64 Visual Studio developer shell; assertions remain enabled.
param([string]$OutputDirectory = 'build/native-queue-asan')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
Push-Location $repo
try {
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    $sources = [ordered]@{
        QueueBuffersTests = 'shared/dispatch/tests/QueueBuffersTests.cpp'
        NativeBackendLifecycleTests = 'bridge-events/tests/NativeBackendLifecycleWinTests.cpp'
        NativeHooksLifecycleTests = 'bridge-events/tests/NativeHooksLifecycleWinTests.cpp'
        NativeDeliveryThreadsTests = 'bridge-events/tests/NativeDeliveryThreadsWinTests.cpp'
    }
    foreach ($suite in $sources.Keys) {
        $source = $sources[$suite]
        $executable = Join-Path $OutputDirectory "$suite.exe"
        $object = Join-Path $OutputDirectory "$suite.obj"
        $symbols = Join-Path $OutputDirectory "$suite.pdb"
        & cl /nologo /std:c++20 /EHsc /MD /Zi /fsanitize=address /W4 /WX /Ibridge-events/include /Ishared/contract /Ishared/ue4ss-abi /Ishared/native /Ishared/tests /Ishared /Ibridge-events/tests/support $source "/Fe$executable" "/Fo$object" "/Fd$symbols"
        if ($LASTEXITCODE -ne 0) { throw "AddressSanitizer $suite build failed" }
        & (Resolve-Path -LiteralPath $executable).Path
        if ($LASTEXITCODE -ne 0) { throw "AddressSanitizer $suite failed" }
    }
    Write-Output 'AddressSanitizer native queue, backend, hook lifecycle and delivery thread tests passed'
} finally { Pop-Location }
