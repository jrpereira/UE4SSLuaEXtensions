import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class VersionResourceTests(unittest.TestCase):
    def test_embedded_lua_literals_stay_within_msvc_limit(self):
        source = (ROOT / 'bridge-events/lua/bridge_api.lua').read_text()
        cmake = (ROOT / 'bridge-events/CMakeLists.txt').read_text()
        template = (ROOT / 'bridge-events/cmake/EmbeddedLuaAPI.hpp.in').read_text()
        size = re.search(r'set\(UE4SSLEB_LUA_API_CHUNK_SIZE (\d+)\)', cmake)
        self.assertIsNotNone(size)
        # MSVC rejects string literals over about 16 KB; keep a wide margin.
        self.assertLessEqual(int(size[1]), 7000)
        self.assertIn('while(UE4SSLEB_LUA_API_OFFSET LESS UE4SSLEB_LUA_API_LENGTH)', cmake)
        self.assertEqual(template.count('@UE4SSLEB_LUA_API_CHUNK_COUNT@'), 1)
        self.assertEqual(template.count('@UE4SSLEB_LUA_API_CHUNKS@'), 1)
        self.assertNotIn(')UE4SSLEB_LUA', source)

    def test_resource_and_packaging_share_canonical_version(self):
        header = (ROOT / 'bridge-events/contract/Version.hpp').read_text()
        match = re.search(r'^#define UE4SSLEB_VERSION "(\d+)\.(\d+)\.(\d+)"$', header, re.M)
        self.assertIsNotNone(match)
        major, minor, patch = match.groups()
        self.assertRegex(header, rf'(?m)^#define UE4SSLEB_VERSION_MAJOR {major}$')
        self.assertRegex(header, rf'(?m)^#define UE4SSLEB_VERSION_MINOR {minor}$')
        self.assertRegex(header, rf'(?m)^#define UE4SSLEB_VERSION_PATCH {patch}$')

        template = (ROOT / 'bridge-events/resources/Version.rc.in').read_text()
        rendered = (template
            .replace('@PROJECT_VERSION_MAJOR@', major)
            .replace('@PROJECT_VERSION_MINOR@', minor)
            .replace('@PROJECT_VERSION_PATCH@', patch)
            .replace('@UE4SSLEB_PRODUCT_VERSION@', '.'.join(match.groups())))
        self.assertIn(f'FILEVERSION {major},{minor},{patch},0', rendered)
        self.assertIn(f'VALUE "ProductVersion", "{major}.{minor}.{patch}\\0"', rendered)
        version = '.'.join(match.groups())
        self.assertIn(f'VALUE "OriginalFilename", "UE4SSLuaEventBridge-{version}.dll\\0"', rendered)

        bridge_cmake = (ROOT / 'bridge-events/CMakeLists.txt').read_text()
        bootstrap_cmake = (ROOT / 'shared/bootstrap/cmake/BridgeBootstrap.cmake').read_text()
        self.assertIn('resources/Version.rc.in', bridge_cmake)
        self.assertIn('UE4SSLuaEventBridge-${UE4SSLEB_PRODUCT_VERSION}', bridge_cmake)
        self.assertIn('resources/Version.rc.in', bootstrap_cmake)

        selector = (ROOT / 'bridge-events/packaging/main.json.in').read_text()
        self.assertIn('"schema": 1', selector)
        self.assertIn('"version": "@UE4SSLEB_PRODUCT_VERSION@"', selector)

        packager = (ROOT / 'tools/package-release.ps1').read_text()
        self.assertIn("test-dll-version.ps1') -Dll $bootstrap", packager)
        self.assertIn("test-dll-version.ps1') -Dll $implementation", packager)

        # Both workflows verify DLL versions through the per-product CI packager.
        ci_packager = (ROOT / 'tools/ci-package-product.ps1').read_text()
        self.assertEqual(ci_packager.count('test-dll-version.ps1'), 2)
        for workflow in ['build.yml', 'release.yml']:
            source = (ROOT / '.github/workflows' / workflow).read_text()
            self.assertIn('ci-package-product.ps1', source)

    def test_event_bridge_bootstrap_keeps_its_identity(self):
        call = (ROOT / 'bridge-events/CMakeLists.txt').read_text()
        call = call[call.index('add_bridge_bootstrap('):]
        call = call[:call.index('\n)')]
        arguments = dict(re.findall(r'^\s+([A-Z_]+) ("[^"]*"|\S+)$', call, re.M))
        arguments = {key: value.strip('"') for key, value in arguments.items()}
        self.assertEqual(arguments['PRODUCT'], 'UE4SSLuaEventBridge')
        self.assertEqual(arguments['TARGET'], 'UE4SSLEBBootstrap')
        self.assertEqual(arguments['DISPLAY_NAME'], 'UE4SS Lua Event Bridge')
        self.assertEqual(arguments['MAGIC'], '0x4C454231')
        self.assertEqual(arguments['CLAIM_NAME'], 'UE4SSLEB')
        self.assertEqual(arguments['DIST_FOLDER'], '0_ModCore_UE4SSLuaEventBridge')

        template = (ROOT / 'shared/bootstrap/resources/Version.rc.in').read_text()
        rendered = (template
            .replace('@BRIDGE_BOOTSTRAP_VERSION_MAJOR@', '1')
            .replace('@BRIDGE_BOOTSTRAP_VERSION_MINOR@', '0')
            .replace('@BRIDGE_BOOTSTRAP_VERSION_PATCH@', '12')
            .replace('@BRIDGE_BOOTSTRAP_VERSION@', '1.0.12')
            .replace('@BRIDGE_BOOTSTRAP_PRODUCT@', arguments['PRODUCT'])
            .replace('@BRIDGE_BOOTSTRAP_DISPLAY_NAME@', arguments['DISPLAY_NAME'])
            .replace('@BRIDGE_BOOTSTRAP_TARGET@', arguments['TARGET']))
        self.assertNotIn('@', rendered)
        for line in [
            'FILEVERSION 1,0,12,0',
            'PRODUCTVERSION 1,0,12,0',
            'VALUE "CompanyName", "UE4SS Lua Event Bridge contributors\\0"',
            'VALUE "FileDescription", "UE4SS Lua Event Bridge bootstrap\\0"',
            'VALUE "FileVersion", "1.0.12.0\\0"',
            'VALUE "InternalName", "UE4SSLEBBootstrap\\0"',
            'VALUE "OriginalFilename", "main.dll\\0"',
            'VALUE "ProductName", "UE4SSLuaEventBridge Bootstrap\\0"',
            'VALUE "ProductVersion", "1.0.12\\0"',
            'VALUE "BridgeRole", "Bootstrap\\0"',
        ]:
            self.assertIn(line, rendered)


if __name__ == '__main__':
    unittest.main()
