"""Pins the event bridge's runtime identity: the bootstrap and the installed
mod folder must keep these values, because installed copies and consumers
depend on them. tests/run.sh runs this with the shared product checks."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
EXTENSION = ROOT / 'bridge-events'
spec = importlib.util.spec_from_file_location('product_checks', ROOT / 'shared/tests/product_checks.py')
product_checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(product_checks)


class IdentityTests(unittest.TestCase):
    def test_bootstrap_identity(self):
        cmake = (EXTENSION / 'CMakeLists.txt').read_text(encoding='utf-8')
        _, identity = product_checks.cmake_call(cmake, 'add_bridge_bootstrap')
        self.assertEqual(identity['PRODUCT'], 'UE4SSLuaEventBridge')
        self.assertEqual(identity['TARGET'], 'UE4SSLEBBootstrap')
        self.assertEqual(identity['DISPLAY_NAME'], 'UE4SS Lua Event Bridge')
        self.assertEqual(product_checks.resolve(cmake, identity['MAGIC']), '0x4C454231')
        self.assertEqual(identity['CLAIM_NAME'], 'UE4SSLEB')
        self.assertEqual(identity['DIST_FOLDER'], '0_ModCore_UE4SSLuaEventBridge')

        template = (ROOT / 'shared/bootstrap/resources/Version.rc.in').read_text(encoding='utf-8')
        rendered = (template
            .replace('@BRIDGE_BOOTSTRAP_VERSION_MAJOR@', '1')
            .replace('@BRIDGE_BOOTSTRAP_VERSION_MINOR@', '2')
            .replace('@BRIDGE_BOOTSTRAP_VERSION_PATCH@', '3')
            .replace('@BRIDGE_BOOTSTRAP_VERSION@', '1.2.3')
            .replace('@BRIDGE_BOOTSTRAP_PRODUCT@', identity['PRODUCT'])
            .replace('@BRIDGE_BOOTSTRAP_DISPLAY_NAME@', identity['DISPLAY_NAME'])
            .replace('@BRIDGE_BOOTSTRAP_TARGET@', identity['TARGET']))
        self.assertNotIn('@', rendered)
        for line in [
            'FILEVERSION 1,2,3,0',
            'VALUE "CompanyName", "UE4SS Lua Event Bridge contributors\\0"',
            'VALUE "FileDescription", "UE4SS Lua Event Bridge bootstrap\\0"',
            'VALUE "InternalName", "UE4SSLEBBootstrap\\0"',
            'VALUE "OriginalFilename", "main.dll\\0"',
            'VALUE "ProductName", "UE4SSLuaEventBridge Bootstrap\\0"',
            'VALUE "ProductVersion", "1.2.3\\0"',
            'VALUE "BridgeRole", "Bootstrap\\0"',
        ]:
            self.assertIn(line, rendered)

    def test_lua_global_and_embedding(self):
        cmake = (EXTENSION / 'CMakeLists.txt').read_text(encoding='utf-8')
        _, embed = product_checks.cmake_call(cmake, 'lex_embed_lua')
        self.assertEqual(embed['NAMESPACE'], 'UE4SSLuaEventBridge')
        self.assertEqual(embed['DELIMITER'], 'UE4SSLEB_LUA')


if __name__ == '__main__':
    unittest.main()
