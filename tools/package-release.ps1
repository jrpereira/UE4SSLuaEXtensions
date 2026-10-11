param([Parameter(Mandatory)][string]$Product,
      [Parameter(Mandatory)][string]$Distribution,
      [Parameter(Mandatory)][string]$OutputDirectory,
      [Parameter(Mandatory)][string]$SourceCommit,
      [switch]$AllowNonPeTestFixture)
$ErrorActionPreference='Stop'
if($SourceCommit -notmatch '^[0-9a-fA-F]{40}$'){throw 'SourceCommit must be a full Git commit SHA'}
$metadata=& (Join-Path $PSScriptRoot 'release-metadata.ps1') -Product $Product
$repo=Split-Path $PSScriptRoot
$root=(Resolve-Path -LiteralPath $Distribution).Path
$folder=$metadata.folder
$implementationName="$($metadata.product_name)-$($metadata.version).dll"
$bootstrap=Join-Path $root 'dlls/main.dll'
$implementation=Join-Path $root "dlls/versions/$implementationName"
$configuration=Join-Path $root 'dlls/main.json'
if(-not (Test-Path -LiteralPath $configuration -PathType Leaf)){throw 'Missing package payload: dlls/main.json'}
$selection=Get-Content -LiteralPath $configuration -Raw|ConvertFrom-Json
$selectionKeys=@($selection.PSObject.Properties.Name)
if($selectionKeys.Count -ne 2 -or 'schema' -cnotin $selectionKeys -or 'version' -cnotin $selectionKeys -or
   $selection.schema -ne 1 -or $selection.version -cne $metadata.version){
    throw 'main.json must select the packaged implementation version'
}
if(-not $AllowNonPeTestFixture){
    & (Join-Path $PSScriptRoot 'test-dll-version.ps1') -Dll $bootstrap -ExpectedVersion $metadata.version `
        -ExpectedProductName "$($metadata.product_name) Bootstrap" -ExpectedOriginalFilename 'main.dll' | Out-Null
    & (Join-Path $PSScriptRoot 'test-dll-version.ps1') -Dll $implementation -ExpectedVersion $metadata.version `
        -ExpectedProductName $metadata.product_name -ExpectedOriginalFilename $implementationName | Out-Null
}

# The packaged mod.json is the product's source manifest with "version" set from Version.hpp.
# A copy the build already staged is used instead, but its version must agree.
$stagedManifest=Join-Path $root 'mod.json'
$sourceManifest=if(Test-Path -LiteralPath $stagedManifest -PathType Leaf){$stagedManifest}else{Join-Path $repo "$Product/mod.json"}
if(-not (Test-Path -LiteralPath $sourceManifest -PathType Leaf)){throw "Missing product manifest: $Product/mod.json"}
$module=Get-Content -LiteralPath $sourceManifest -Raw -Encoding utf8|ConvertFrom-Json
if($module -isnot [Management.Automation.PSCustomObject]){throw 'mod.json must be a JSON object'}
$existing=$module.PSObject.Properties['version']
if($existing -and $existing.Value -cne $metadata.version){
    throw "mod.json version $($existing.Value) does not match the product version $($metadata.version)"
}
$packagedModule=[ordered]@{}
foreach($property in $module.PSObject.Properties){
    # version_source names a repository path that does not exist once installed.
    if($property.Name -ceq 'version' -or $property.Name -ceq 'version_source'){continue}
    $packagedModule[$property.Name]=$property.Value
    if($property.Name -ceq 'name'){$packagedModule['version']=$metadata.version}
}
if(-not $packagedModule.Contains('version')){$packagedModule['version']=$metadata.version}
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
    New-Item -ItemType Directory -Force $OutputDirectory|Out-Null
    $archive=Join-Path $OutputDirectory $metadata.archive
    foreach($path in @($archive,"$archive.manifest.json","$archive.sha256")){
        if(Test-Path -LiteralPath $path){throw "Refusing to overwrite existing artifact: $path"}
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
& (Join-Path $PSScriptRoot 'test-release-archive.ps1') -Product $Product -Archive $archive
$manifest=[ordered]@{schema=1;product=$metadata.product_name;version=$metadata.version}
if($null -ne $metadata.api){$manifest['api']=$metadata.api}
$manifest['source_commit']=$SourceCommit
$manifest['target_ue4ss_commit']='97b7e501'
$manifest['files']=$files
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath "$archive.manifest.json" -Encoding utf8
$digest=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$digest  $($metadata.archive)"|Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
[pscustomobject]@{archive=$archive;manifest="$archive.manifest.json";checksum="$archive.sha256"}
