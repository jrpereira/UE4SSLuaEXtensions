"""The packaging tools verify both DLLs' version resources for every product.
Per-extension version, resource and identity checks are shared/tests/product_checks.py
and each extension's tests/test_identity.py."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackagingToolTests(unittest.TestCase):
    def test_packager_checks_both_dll_versions(self):
        packager = (ROOT / 'tools/package-release.ps1').read_text()
        self.assertIn("test-dll-version.ps1') -Dll $bootstrap", packager)
        self.assertIn("test-dll-version.ps1') -Dll $implementation", packager)
        bootstrap_cmake = (ROOT / 'shared/bootstrap/cmake/BridgeBootstrap.cmake').read_text()
        self.assertIn('resources/Version.rc.in', bootstrap_cmake)

    def test_workflows_package_through_the_ci_packager(self):
        ci_packager = (ROOT / 'tools/ci-package-product.ps1').read_text()
        self.assertEqual(ci_packager.count('test-dll-version.ps1'), 2)
        for workflow in ['build.yml', 'release.yml']:
            source = (ROOT / '.github/workflows' / workflow).read_text()
            self.assertIn('ci-package-product.ps1', source)


if __name__ == '__main__':
    unittest.main()
