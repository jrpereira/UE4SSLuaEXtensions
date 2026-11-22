param([Parameter(Mandatory)][string]$Tag,
      [Parameter(Mandatory)][string]$SourceCommit,
      [string]$OutputFile)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
$products=@{
    'bridge-events'=[ordered]@{name='UE4SSLuaEventBridge';api=6}
    'bridge-files'=[ordered]@{name='UE4SSLuaFileBridge';api=$null}
    'bridge-sounds'=[ordered]@{name='UE4SSLuaSoundBridge';api=$null}
    'bridge-logs'=[ordered]@{name='UE4SSLuaLogBridge';api=$null}
}
if($Tag -cnotmatch '^(bridge-[a-z]+)/v[0-9]+\.[0-9]+\.[0-9]+$'){throw "Tag must be <product>/vMAJOR.MINOR.PATCH: $Tag"}
$product=$Matches[1]
if(-not $products.Contains($product)){throw "Unknown product: $product"}
if($SourceCommit -notmatch '^[0-9a-fA-F]{40}$'){throw 'SourceCommit must be a full Git commit SHA'}
$name=$products[$product].name
$api=$products[$product].api

$header=Join-Path $repo "$product/contract/Version.hpp"
if(-not (Test-Path -LiteralPath $header -PathType Leaf)){throw "Missing product version header: $product/contract/Version.hpp"}
$found=@([regex]::Matches((Get-Content -LiteralPath $header -Raw),'(?m)^#define [A-Z][A-Z0-9_]*_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"\s*$'))
if($found.Count -ne 1){throw 'Invalid product version'}
$version=$found[0].Groups[1].Value
if($Tag -cne "$product/v$version"){throw "$product/contract/Version.hpp gives $product/v$version, not $Tag"}

$folder="0_ModCore_$name"
$archiveName="$name-v$version.zip"
$implementationName="$name-$version.dll"
$root=(Resolve-Path -LiteralPath (Join-Path $repo "build/dist/$folder")).Path
$bootstrap=Join-Path $root 'dlls/main.dll'
$implementation=Join-Path $root "dlls/versions/$implementationName"
$configuration=Join-Path $root 'dlls/main.json'

function Test-DllVersion([string]$Dll,[string]$ProductName,[string]$OriginalFilename){
    $info=(Get-Item -LiteralPath (Resolve-Path -LiteralPath $Dll).Path).VersionInfo
    $expected=@($version.Split('.')|ForEach-Object{[int]$_})+@(0)
    $actual=@($info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart,$info.FilePrivatePart)
    if(Compare-Object $expected $actual -SyncWindow 0){
        throw "DLL FileVersion mismatch: expected $($expected -join '.'), got $($actual -join '.')"
    }
    if($info.ProductVersion -cne $version){throw "DLL ProductVersion mismatch: expected $version, got $($info.ProductVersion)"}
    if($info.ProductName -cne $ProductName){throw "Unexpected DLL product name: $($info.ProductName)"}
    if($info.OriginalFilename -cne $OriginalFilename){throw "Unexpected original filename: $($info.OriginalFilename)"}
}
Test-DllVersion $bootstrap "$name Bootstrap" 'main.dll'
Test-DllVersion $implementation $name $implementationName

if(-not (Test-Path -LiteralPath $configuration -PathType Leaf)){throw 'Missing package payload: dlls/main.json'}
$selection=Get-Content -LiteralPath $configuration -Raw|ConvertFrom-Json
$selectionKeys=@($selection.PSObject.Properties.Name)
if($selectionKeys.Count -ne 2 -or 'schema' -cnotin $selectionKeys -or 'version' -cnotin $selectionKeys -or
   $selection.schema -ne 1 -or $selection.version -cne $version){
    throw 'main.json must select the packaged implementation version'
}

$stagedManifest=Join-Path $root 'mod.json'
$sourceManifest=if(Test-Path -LiteralPath $stagedManifest -PathType Leaf){$stagedManifest}else{Join-Path $repo "$product/mod.json"}
if(-not (Test-Path -LiteralPath $sourceManifest -PathType Leaf)){throw "Missing product manifest: $product/mod.json"}
$module=Get-Content -LiteralPath $sourceManifest -Raw -Encoding utf8|ConvertFrom-Json
if($module -isnot [Management.Automation.PSCustomObject]){throw 'mod.json must be a JSON object'}
$existing=$module.PSObject.Properties['version']
if($existing -and $existing.Value -cne $version){throw "mod.json version $($existing.Value) does not match the product version $version"}
$packagedModule=[ordered]@{}
foreach($property in $module.PSObject.Properties){
    if($property.Name -ceq 'version' -or $property.Name -ceq 'version_source'){continue}
    $packagedModule[$property.Name]=$property.Value
    if($property.Name -ceq 'name'){$packagedModule['version']=$version}
}
if(-not $packagedModule.Contains('version')){$packagedModule['version']=$version}

$archive=Join-Path $repo $archiveName
foreach($path in @($archive,"$archive.manifest.json","$archive.sha256")){
    if(Test-Path -LiteralPath $path){throw "Refusing to overwrite existing artifact: $path"}
}
$staging=Join-Path ([IO.Path]::GetTempPath()) ('package-manifest-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $staging|Out-Null
try {
    $moduleFile=Join-Path $staging 'mod.json'
    [IO.File]::WriteAllText($moduleFile,(($packagedModule|ConvertTo-Json -Depth 10)+"`n"),[Text.UTF8Encoding]::new($false))
    $sources=[ordered]@{
        'enabled.txt'=Join-Path $root 'enabled.txt'
        'mod.json'=$moduleFile
        'mod_settings.ini'=Join-Path $root 'mod_settings.ini'
        'Scripts/main.lua'=Join-Path $root 'Scripts/main.lua'
        'Scripts/lex_verify.lua'=Join-Path $root 'Scripts/lex_verify.lua'
        'dlls/main.dll'=$bootstrap
        'dlls/main.json'=$configuration
        "dlls/versions/$implementationName"=$implementation
    }
    $files=@()
    foreach($relative in $sources.Keys){
        $path=$sources[$relative]
        if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw "Missing package payload: $relative"}
        $files+=@{path="$folder/$relative";sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip=[IO.Compression.ZipFile]::Open($archive,[IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach($relative in $sources.Keys){
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$sources[$relative],"$folder/$relative")|Out-Null
        }
    } finally {$zip.Dispose()}
} finally {Remove-Item -LiteralPath $staging -Recurse -Force}

$zip=[IO.Compression.ZipFile]::OpenRead($archive)
try {
    $allowed=@($sources.Keys|ForEach-Object{"$folder/$_"})
    $entries=@($zip.Entries|ForEach-Object{$_.FullName})
    if($entries.Count -ne $allowed.Count -or (Compare-Object $allowed $entries -CaseSensitive)){throw 'Unexpected package entries'}
    if($entries|Where-Object{$_.Replace('\','/') -match '(?i)(^|/)tools/'}){throw 'Forbidden Tools directory'}
    $reader=[IO.StreamReader]::new($zip.GetEntry("$folder/mod.json").Open())
    try {$packaged=$reader.ReadToEnd()|ConvertFrom-Json} finally {$reader.Dispose()}
    if($packaged.version -cne $version){throw "Packaged mod.json version must be $version"}
    if($packaged.PSObject.Properties['version_source']){throw 'Packaged mod.json must not name a repository version source'}
} finally {$zip.Dispose()}

$manifest=[ordered]@{schema=1;product=$name;version=$version}
if($null -ne $api){$manifest['api']=$api}
$manifest['source_commit']=$SourceCommit
$manifest['target_ue4ss_commit']='97b7e501'
$manifest['files']=$files
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$archive.manifest.json" -Encoding utf8
$digest=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$digest  $archiveName"|Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
if($OutputFile){"archive=$archiveName"|Add-Content -LiteralPath $OutputFile -Encoding utf8}
