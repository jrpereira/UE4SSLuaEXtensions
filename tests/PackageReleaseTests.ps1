$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
$temporaryRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture=Join-Path $temporaryRoot ('bridge-package-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $fixture|Out-Null
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-Distribution([string]$Path, [string]$ProductName, [string]$Version) {
    New-Item -ItemType Directory -Force (Join-Path $Path 'dlls/versions')|Out-Null
    'enabled'|Set-Content (Join-Path $Path 'enabled.txt')
    New-Item -ItemType Directory -Force (Join-Path $Path 'Scripts')|Out-Null
    '-- verifications'|Set-Content (Join-Path $Path 'Scripts/main.lua')
    '-- harness'|Set-Content (Join-Path $Path 'Scripts/lex_verify.lua')
    '[Mod]'|Set-Content (Join-Path $Path 'mod_settings.ini')
    [IO.File]::WriteAllBytes((Join-Path $Path 'dlls/main.dll'),[byte[]](1,2,3,4))
    "{`"schema`":1,`"version`":`"$Version`"}"|Set-Content (Join-Path $Path 'dlls/main.json')
    [IO.File]::WriteAllBytes((Join-Path $Path "dlls/versions/$ProductName-$Version.dll"),[byte[]](5,6,7,8))
    'must not ship'|Set-Content (Join-Path $Path 'personal-config.json')
}

function Read-Entry($Zip, [string]$Name) {
    $reader=[IO.StreamReader]::new($Zip.GetEntry($Name).Open())
    try {$reader.ReadToEnd()} finally {$reader.Dispose()}
}

function Assert-Package($Result, [string]$Folder, [string]$Version, [string]$Id) {
    $manifest=Get-Content -LiteralPath $Result.manifest -Raw|ConvertFrom-Json
    $zip=[IO.Compression.ZipFile]::OpenRead($Result.archive)
    try {
        if($zip.Entries.Count -ne 8){throw 'Unexpected payload count'}
        if(@($manifest.files).Count -ne 8){throw 'Unexpected manifest file count'}
        foreach($file in $manifest.files){
            $entry=$zip.GetEntry($file.path)
            if(-not $entry){throw "Manifest entry missing: $($file.path)"}
            $stream=$entry.Open()
            $hasher=[Security.Cryptography.SHA256]::Create()
            try {$hash=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}
            finally {$hasher.Dispose();$stream.Dispose()}
            if($hash -ne $file.sha256){throw 'Manifest hash mismatch'}
        }
        if($zip.GetEntry("$Folder/personal-config.json")){throw 'Personal configuration shipped'}
        $module=(Read-Entry $zip "$Folder/mod.json")|ConvertFrom-Json
        if($module.version -cne $Version){throw "Packaged mod.json version must be $Version"}
        if($module.id -cne $Id){throw 'Packaged mod.json lost its fields'}
        if($module.PSObject.Properties['version_source']){throw 'Packaged mod.json kept version_source'}
    } finally {$zip.Dispose()}
    if(-not (Get-Content -LiteralPath $Result.checksum -Raw).StartsWith((Get-FileHash $Result.archive -Algorithm SHA256).Hash.ToLowerInvariant())){throw 'Archive checksum mismatch'}
    $manifest
}

function Assert-Rejected([scriptblock]$Action, [string]$Description, [string]$Message) {
    $rejected=$false
    try { & $Action|Out-Null } catch {
        if($Message -and $_.Exception.Message -notlike $Message){throw}
        $rejected=$true
    }
    if(-not $rejected){throw "Accepted: $Description"}
}

try {
    # Event bridge: mod.json comes from bridge-events/mod.json with the version filled in.
    $distribution=Join-Path $fixture 'dist'
    $version=(& (Join-Path $repo 'tools/release-metadata.ps1') -Product bridge-events).version
    New-Distribution $distribution 'UE4SSLuaEventBridge' $version
    $packager=Join-Path $repo 'tools/package-release.ps1'
    $result=& $packager -Product bridge-events -Distribution $distribution -OutputDirectory (Join-Path $fixture 'out') -SourceCommit ('a'*40) -AllowNonPeTestFixture
    $manifest=Assert-Package $result '0_ModCore_UE4SSLuaEventBridge' $version 'UE4SSLuaEventBridge'
    if($manifest.api -ne 5){throw 'Package API version mismatch'}
    if($manifest.product -cne 'UE4SSLuaEventBridge'){throw 'Package product mismatch'}
    $source=Get-Content -LiteralPath (Join-Path $repo 'bridge-events/mod.json') -Raw|ConvertFrom-Json
    $zip=[IO.Compression.ZipFile]::OpenRead($result.archive)
    try {$packaged=(Read-Entry $zip '0_ModCore_UE4SSLuaEventBridge/mod.json')|ConvertFrom-Json} finally {$zip.Dispose()}
    foreach($property in $source.PSObject.Properties){
        if($property.Name -in @('version','version_source')){continue}
        if(($packaged.$($property.Name)|ConvertTo-Json -Depth 10 -Compress) -cne ($property.Value|ConvertTo-Json -Depth 10 -Compress)){
            throw "Packaged mod.json changed field $($property.Name)"
        }
    }

    Assert-Rejected { & $packager -Product bridge-events -Distribution $distribution -OutputDirectory (Join-Path $fixture 'out') -SourceCommit ('a'*40) -AllowNonPeTestFixture } 'overwriting an existing artifact'

    # A mod.json the build already staged is used, but its version must agree.
    '{"id":"UE4SSLuaEventBridge","name":"Staged","version":"0.0.1"}'|Set-Content (Join-Path $distribution 'mod.json')
    Assert-Rejected { & $packager -Product bridge-events -Distribution $distribution -OutputDirectory (Join-Path $fixture 'stale') -SourceCommit ('a'*40) -AllowNonPeTestFixture } 'stale staged mod.json' 'mod.json version*'
    "{`"id`":`"UE4SSLuaEventBridge`",`"name`":`"Staged`",`"version`":`"$version`"}"|Set-Content (Join-Path $distribution 'mod.json')
    $staged=& $packager -Product bridge-events -Distribution $distribution -OutputDirectory (Join-Path $fixture 'staged') -SourceCommit ('a'*40) -AllowNonPeTestFixture
    Assert-Package $staged '0_ModCore_UE4SSLuaEventBridge' $version 'UE4SSLuaEventBridge'|Out-Null
    Remove-Item -LiteralPath (Join-Path $distribution 'mod.json')

    '{"schema":1,"version":"auto"}'|Set-Content (Join-Path $distribution 'dlls/main.json')
    Assert-Rejected { & $packager -Product bridge-events -Distribution $distribution -OutputDirectory (Join-Path $fixture 'bad-selector') -SourceCommit ('a'*40) -AllowNonPeTestFixture } 'non-exact packaged selector' 'main.json must select*'
    Assert-Rejected { & $packager -Product unknown -Distribution $distribution -OutputDirectory (Join-Path $fixture 'unknown') -SourceCommit ('a'*40) -AllowNonPeTestFixture } 'unknown product' 'Unknown product*'

    $validator=Join-Path $repo 'tools/test-release-archive.ps1'
    & $validator -Product bridge-events -Archive $result.archive
    foreach($forbidden in @('Tools/a.txt','0_ModCore_UE4SSLuaEventBridge/tOoLs/a.txt','0_ModCore_UE4SSLuaEventBridge/x/TOOLS/','0_ModCore_UE4SSLuaEventBridge\Tools\a.txt')){
        $bad=Join-Path $fixture ([guid]::NewGuid().ToString('N')+'.zip')
        Copy-Item -LiteralPath $result.archive -Destination $bad
        $zip=[IO.Compression.ZipFile]::Open($bad,[IO.Compression.ZipArchiveMode]::Update)
        try {$zip.CreateEntry($forbidden)|Out-Null} finally {$zip.Dispose()}
        Assert-Rejected { & $validator -Product bridge-events -Archive $bad } "forbidden Tools entry $forbidden" 'Forbidden Tools directory:*'
    }
    $bad=Join-Path $fixture ([guid]::NewGuid().ToString('N')+'.zip')
    Copy-Item -LiteralPath $result.archive -Destination $bad
    $zip=[IO.Compression.ZipFile]::Open($bad,[IO.Compression.ZipArchiveMode]::Update)
    try {$zip.GetEntry('0_ModCore_UE4SSLuaEventBridge/mod.json').Delete()} finally {$zip.Dispose()}
    Assert-Rejected { & $validator -Product bridge-events -Archive $bad } 'archive without mod.json'

    # A second product packages under its own folder and names.
    $other=Join-Path $fixture 'repo'
    New-Item -ItemType Directory -Force (Join-Path $other 'tools'),(Join-Path $other 'bridge-files/contract')|Out-Null
    foreach($tool in @('release-metadata.ps1','package-release.ps1','test-release-archive.ps1','test-dll-version.ps1')){
        Copy-Item -LiteralPath (Join-Path $repo "tools/$tool") -Destination (Join-Path $other 'tools')
    }
    "#define UE4SSLFB_VERSION `"0.1.0`"`n"|Set-Content (Join-Path $other 'bridge-files/contract/Version.hpp')
    '{"id":"UE4SSLuaFileBridge","name":"UE4SS Lua File Bridge","version_source":"contract/Version.hpp"}'|Set-Content (Join-Path $other 'bridge-files/mod.json')
    $filesDistribution=Join-Path $fixture 'files-dist'
    New-Distribution $filesDistribution 'UE4SSLuaFileBridge' '0.1.0'
    $files=& (Join-Path $other 'tools/package-release.ps1') -Product bridge-files -Distribution $filesDistribution -OutputDirectory (Join-Path $fixture 'files-out') -SourceCommit ('b'*40) -AllowNonPeTestFixture
    if([IO.Path]::GetFileName($files.archive) -cne 'UE4SSLuaFileBridge-v0.1.0.zip'){throw 'File bridge archive name mismatch'}
    $filesManifest=Assert-Package $files '0_ModCore_UE4SSLuaFileBridge' '0.1.0' 'UE4SSLuaFileBridge'
    if($filesManifest.product -cne 'UE4SSLuaFileBridge'){throw 'File bridge manifest product mismatch'}
    Assert-Rejected { & $validator -Product bridge-events -Archive $files.archive } 'one product''s archive validated as another'

    'Package allowlist, mod.json version, Tools rejection, hashes and overwrite protection passed'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(-not $resolved.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase) -or
       [IO.Path]::GetFileName($resolved) -notlike 'bridge-package-test-*'){throw 'Unsafe test cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
