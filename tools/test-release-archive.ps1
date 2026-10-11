param([Parameter(Mandatory)][string]$Product, [Parameter(Mandatory)][string]$Archive)
$ErrorActionPreference = 'Stop'
$metadata=& (Join-Path $PSScriptRoot 'release-metadata.ps1') -Product $Product
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Archive).Path)
try {
    $folder = $metadata.folder
    $allowed = @(
        "$folder/enabled.txt",
        "$folder/mod.json",
        "$folder/mod_settings.ini",
        "$folder/Scripts/main.lua",
        "$folder/dlls/main.dll",
        "$folder/dlls/main.json",
        "$folder/dlls/versions/$($metadata.product_name)-$($metadata.version).dll"
    )
    foreach ($entry in $zip.Entries) {
        $normalized = $entry.FullName.Replace('\', '/')
        if ($normalized -match '(?i)(^|/)tools/') { throw "Forbidden Tools directory: $($entry.FullName)" }
        if ($entry.FullName -cnotin $allowed) { throw "Unexpected package entry: $($entry.FullName)" }
    }
    if ($zip.Entries.Count -ne $allowed.Count) { throw 'Unexpected package entry count' }
    foreach ($name in $allowed) {
        if (@($zip.Entries | Where-Object FullName -CEQ $name).Count -ne 1) { throw "Missing or duplicate package entry: $name" }
    }
    $reader = [IO.StreamReader]::new($zip.GetEntry("$folder/mod.json").Open())
    try { $module = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($module.version -cne $metadata.version) { throw "Packaged mod.json version must be $($metadata.version)" }
    if ($module.PSObject.Properties['version_source']) { throw 'Packaged mod.json must not name a repository version source' }
} finally { $zip.Dispose() }
