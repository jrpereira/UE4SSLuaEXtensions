# Changelog

Changes to the package's own folder, `0_ModCore_UE4SSLuaEXtensions`. Each
extension keeps its own changelog in its folder.

## v1.3.3

### Changes

- Fix the extensions menu failing at startup on a fresh install with "cannot
  create the cache directory". The menu now creates its `cache` folders through
  UE4SSLuaFileBridge when it is loaded, falls back to the shell, and judges
  success by writing a file into the folder instead of trusting the command's
  result. If it still fails, the extensions keep working and are listed on
  their own, as before.
