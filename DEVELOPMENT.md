# Developing UE4SS Lua Extensions

For contributors to the extensions themselves. Mod authors who use them want the
[README](README.md).

## Repository layout

```text
bridge-events/, bridge-files/,  one folder per extension: src/, include/, lua/, contract/Version.hpp,
bridge-sounds/, bridge-logs/    packaging/, tests/, docs/, examples/, CHANGELOG.md, mod.json
extensions-menu/                the package's own Lua folder (no DLL): the extensions as one ModCore Settings module
shared/
  bootstrap/                    the generic main.dll that selects and starts an extension's versioned DLL
  contract/                     ImplementationABI.h, the private ABI between the bootstrap and a DLL
  dispatch/                     queue and dispatch code both extensions build against
  ue4ss-abi/                    the pinned UE4SS 97b7e501 headers and import list every extension uses
  native/                       UE4SS.log reporting and the descriptor export
  cmake/                        LexProduct.cmake: Lua embedding, version resource, selector, import library
  templates/                    Implementation.rc.in, main.json.in, EmbeddedLuaAPI.hpp.in
  lua/                          lex_verify.lua, the harness of every extension's load-time verifications
  tests/                        checks every extension must pass, TestSupport.hpp (CHECK), TestKit.lua
tools/                          build, packaging, release and hygiene tooling
tests/                          run.sh (the test runner) and the tooling's own tests
package-manifest.json           what the combined package ZIP holds and where each file goes
CHANGELOG.md                    changes to the package's own folder; its section for a package version opens that release's notes
repository-policy.json          vendored files and their source modules, for the hygiene check
```

An extension's folder holds only what is specific to it. Its `CMakeLists.txt`
calls the shared functions with its own name, Lua API and descriptor magic.

Menu rows live in the extension's `mod.json`, as a `settings` array, and
appear in array order:

```json
{ "id": "GlobalLevel", "type": "picker", "label": "Global log level", "group": "Levels",
  "values": [[1, "Trace"], [2, "Debug"]], "default": 4 }
```

A value is stored where the menu stores a row by default: `config.ini`,
section `[<mod.json id>]`, key `<row id>` (requested from ModCore Settings,
COORDINATOR/MCS/REQ-config-defaults.md). `file`, `section` and `key` override
that only when needed and are the only storage fields written. The build writes
the rows to the mod folder's `mod_settings.ini`
(`shared/bootstrap/cmake/ModSettings.cmake`), which the menus read; there is no
`mod_settings.ini` source file.

## Tests

```sh
bash tests/run.sh                 # shared code, then every extension
bash tests/run.sh bridge-files    # shared code and one extension
bash tests/run.sh --only lua      # cpp | lua | python | checks
```

For each extension the runner runs the shared checks (`shared/tests`: version
and identity, the ABI syntax audit, the load-time verification gate), then the
extension's own suites in `<extension>/tests`: `*Tests.cpp`, `*Tests.lua` and
`test_*.py`. Suites named `*WinTests.cpp` need Windows and run in CI. The
tooling's tests are `python3 -m unittest discover -s tests` and the PowerShell
suites in `tests/`.

DLLs are built only by GitHub Actions. Build details are in the
[event bridge maintainer guide](bridge-events/docs/BUILD.md).
