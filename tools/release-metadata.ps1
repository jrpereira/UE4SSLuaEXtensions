param([Parameter(Mandatory)][string]$Product, [string]$ReleaseRef, [string]$OutputFile)
$ErrorActionPreference='Stop'
# Each product folder releases on its own: release/<product>/v<version> publishes tag <product>/v<version>.
$products=@{
    'bridge-events'=[ordered]@{name='UE4SSLuaEventBridge';api=6}
    'bridge-files'=[ordered]@{name='UE4SSLuaFileBridge';api=$null}
}
if(-not $products.Contains($Product)){throw "Unknown product: $Product"}
$definition=$products[$Product]
$repo=Split-Path $PSScriptRoot
$header=Join-Path $repo "$Product/contract/Version.hpp"
if(-not (Test-Path -LiteralPath $header -PathType Leaf)){throw "Missing product version header: $Product/contract/Version.hpp"}
$source=Get-Content -LiteralPath $header -Raw
$found=@([regex]::Matches($source,'(?m)^#define [A-Z][A-Z0-9_]*_VERSION "([0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?)"\s*$'))
if($found.Count -ne 1){
    throw 'Invalid product version'
}
$version=$found[0].Groups[1].Value
if($ReleaseRef){
    if($ReleaseRef -cmatch '^release/v'){
        throw "Legacy release/v* branches cannot publish new releases; use release/$Product/v$version"
    }
    if($ReleaseRef -cne "release/$Product/v$version"){
        throw "Release ref must match release/$Product/v$version"
    }
}
$archive="$($definition.name)-v$version.zip"
$result=[ordered]@{
    product=$Product
    product_name=$definition.name
    api=$definition.api
    folder="0_ModCore_$($definition.name)"
    version=$version
    tag="$Product/v$version"
    archive=$archive
    artifact="$($definition.name)-v$version"
    prerelease=$version.Contains('-')
}
if($OutputFile){
    @("product=$Product","product_name=$($result.product_name)","folder=$($result.folder)","version=$version",
      "tag=$($result.tag)","archive=$archive","artifact=$($result.artifact)",
      "prerelease=$($result.prerelease.ToString().ToLowerInvariant())") |
        Add-Content -LiteralPath $OutputFile -Encoding utf8
}
[pscustomobject]$result
