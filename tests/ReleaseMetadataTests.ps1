$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
$script=Join-Path $repo 'tools/release-metadata.ps1'
function Assert-Rejected([scriptblock]$Action, [string]$Description, [string]$Message) {
    $rejected=$false
    try { & $Action | Out-Null } catch {
        if($Message -and $_.Exception.Message -notlike $Message){throw}
        $rejected=$true
    }
    if(-not $rejected){throw "Accepted: $Description"}
}

$actual=& $script -Product bridge-events
if($actual.product_name -cne 'UE4SSLuaEventBridge'){throw 'Event bridge product name changed'}
if($actual.folder -cne '0_ModCore_UE4SSLuaEventBridge'){throw 'Event bridge folder changed'}
if($actual.archive -cne "UE4SSLuaEventBridge-v$($actual.version).zip"){throw 'Archive version mismatch'}
if($actual.artifact -cne "UE4SSLuaEventBridge-v$($actual.version)"){throw 'Artifact name mismatch'}
if($actual.tag -cne "bridge-events/v$($actual.version)"){throw 'Tag must carry the product prefix'}
if($actual.api -ne 6){throw 'Event bridge package API changed'}
if($actual.prerelease -ne $actual.version.Contains('-')){throw 'Prerelease mismatch'}
$released=& $script -Product bridge-events -ReleaseRef "release/bridge-events/v$($actual.version)"
if($released.tag -cne "bridge-events/v$($actual.version)"){throw 'Release tag mismatch'}

Assert-Rejected { & $script -Product bridge-events -ReleaseRef "release/v$($actual.version)" } 'legacy release/v* branch' 'Legacy release/v* branches cannot publish*'
foreach($invalid in @('main','release/v0.0.0','release/v','refs/tags/v0.3.4',"release/bridge-events/v$($actual.version)-rc.1",
                      'release/bridge-events/v0.0.0',"release/bridge-files/v$($actual.version)","release/Bridge-Events/v$($actual.version)",
                      "bridge-events/v$($actual.version)")){
    Assert-Rejected { & $script -Product bridge-events -ReleaseRef $invalid } "release ref $invalid"
}
Assert-Rejected { & $script -Product unknown } 'unknown product' 'Unknown product*'
Assert-Rejected { & $script -Product '../bridge-events' } 'path as product' 'Unknown product*'
if(-not (Test-Path -LiteralPath (Join-Path $repo 'bridge-files'))){
    Assert-Rejected { & $script -Product bridge-files } 'product without a folder' 'Missing product version header*'
}

# A second product reads its own Version.hpp, whatever its macro prefix.
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('release-metadata-test-'+[guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force (Join-Path $fixture 'tools'),(Join-Path $fixture 'bridge-files/contract')|Out-Null
    Copy-Item -LiteralPath $script -Destination (Join-Path $fixture 'tools')
    $copy=Join-Path $fixture 'tools/release-metadata.ps1'
    $header=Join-Path $fixture 'bridge-files/contract/Version.hpp'
    "#pragma once`n#define UE4SSLFB_VERSION `"0.1.0`"`n#define UE4SSLFB_VERSION_MAJOR 0`n"|Set-Content -LiteralPath $header
    $files=& $copy -Product bridge-files -ReleaseRef 'release/bridge-files/v0.1.0'
    if($files.product_name -cne 'UE4SSLuaFileBridge' -or $files.folder -cne '0_ModCore_UE4SSLuaFileBridge'){throw 'File bridge identity mismatch'}
    if($files.tag -cne 'bridge-files/v0.1.0' -or $files.archive -cne 'UE4SSLuaFileBridge-v0.1.0.zip'){throw 'File bridge release names mismatch'}
    Assert-Rejected { & $copy -Product bridge-files -ReleaseRef 'release/bridge-events/v0.1.0' } 'another product''s branch'
    Assert-Rejected { & $copy -Product bridge-files -ReleaseRef 'release/v0.1.0' } 'legacy branch for the file bridge' 'Legacy release/v* branches cannot publish*'
    "#define A_VERSION `"0.1.0`"`n#define B_VERSION `"0.2.0`"`n"|Set-Content -LiteralPath $header
    Assert-Rejected { & $copy -Product bridge-files } 'two version definitions' 'Invalid product version'
    $outputs=Join-Path $fixture 'outputs.txt'
    "#define UE4SSLFB_VERSION `"0.1.0-rc.1`"`n"|Set-Content -LiteralPath $header
    & $copy -Product bridge-files -OutputFile $outputs|Out-Null
    $written=Get-Content -LiteralPath $outputs
    foreach($line in @('product=bridge-files','tag=bridge-files/v0.1.0-rc.1','archive=UE4SSLuaFileBridge-v0.1.0-rc.1.zip',
                       'artifact=UE4SSLuaFileBridge-v0.1.0-rc.1','prerelease=true')){
        if($line -cnotin $written){throw "Missing workflow output: $line"}
    }
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetFileName($resolved) -notlike 'release-metadata-test-*'){throw 'Unsafe test cleanup path'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}

$workflow=Get-Content (Join-Path $repo '.github/workflows/release.yml') -Raw
if($workflow -match 'pull_request|RELEASE_PR_HEAD_SHA'){throw 'Unmerged PR publishing must stay disabled'}
if($workflow -notmatch '--prerelease'){throw 'RC releases must remain prereleases'}
if($workflow -notmatch '--latest=false'){throw 'Product releases must not be marked Latest'}
if($workflow -match '"release/v\*"'){throw 'Legacy release/v* branches must not trigger releases'}
'Release metadata and publication checks passed'
$tagTest=Join-Path $repo 'tools/test-release-tag.ps1'
$commit='a'*40
$tagObject='b'*40
foreach($tag in @('v1.2.3','bridge-events/v1.2.3')){
    & $tagTest -RemoteRefs @() -Tag $tag -ExpectedCommit $commit
    & $tagTest -RemoteRefs @("$commit`trefs/tags/$tag") -Tag $tag -ExpectedCommit $commit
    & $tagTest -RemoteRefs @("$tagObject`trefs/tags/$tag","$commit`trefs/tags/$tag^{}") -Tag $tag -ExpectedCommit $commit
    Assert-Rejected { & $tagTest -RemoteRefs @("$tagObject`trefs/tags/$tag") -Tag $tag -ExpectedCommit $commit } "mismatched existing tag $tag"
}
'Existing lightweight and annotated tag checks passed'
