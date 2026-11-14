import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WorkflowRuntimeTests(unittest.TestCase):
    def test_actions_use_node24_compatible_checkout_and_native_setup(self):
        workflows = list((ROOT / '.github/workflows').glob('*.yml'))
        combined = '\n'.join(path.read_text() for path in workflows)
        self.assertNotIn('actions/checkout@v4', combined)
        self.assertNotIn('actions/setup-python@v5', combined)
        self.assertNotIn('ilammy/msvc-dev-cmd', combined)
        self.assertIn('actions/checkout@v6', combined)
        self.assertIn('actions/setup-python@v6', combined)

        native_script = (ROOT / 'tools/ci-windows-native.ps1').read_text()
        self.assertIn('vswhere.exe', native_script)
        self.assertIn('Microsoft.VisualStudio.Component.VC.Tools.x86.x64', native_script)
        self.assertIn('Microsoft.VisualStudio.DevShell.dll', native_script)


def job_names(workflow):
    """Top-level job ids of a workflow, read without a YAML parser."""
    text = (ROOT/'.github/workflows'/workflow).read_text()
    jobs = text[text.index('\njobs:\n'):]
    return [line[2:-1] for line in jobs.splitlines()
            if line.startswith('  ') and not line.startswith('   ') and line.endswith(':')]


class ReleaseWorkflowTests(unittest.TestCase):
    products = re.findall(r"^    '(bridge-[a-z]+)'=", (ROOT/'tools/release-metadata.ps1').read_text(), re.M)

    def test_required_check_names_are_unchanged(self):
        self.assertEqual(job_names('build.yml'), ['repository-hygiene', 'portable-tests', 'windows-dll'])
        self.assertEqual(job_names('repository-hygiene.yml'), ['repository-hygiene'])

    def test_releases_run_only_on_product_branches(self):
        text = (ROOT/'.github/workflows/release.yml').read_text()
        triggers = re.findall(r'^      - "(release/[^"]+)"$', text, re.M)
        self.assertEqual(triggers, [f'release/{product}/v*' for product in self.products])
        self.assertNotIn('"release/v*"', text)
        self.assertIn(r"-notmatch '^release/(bridge-[a-z]+)/v'", text)
        self.assertIn('-ReleaseRef $env:GITHUB_REF_NAME', text)

    def test_releases_are_attested_and_never_latest(self):
        text = (ROOT/'.github/workflows/release.yml').read_text()
        self.assertIn('uses: actions/attest-build-provenance@v4', text)
        self.assertIn('subject-path: ${{ env.RELEASE_ARCHIVE }}', text)
        job = text[text.index('  windows-release:'):]
        for permission in ['contents: write', 'id-token: write', 'attestations: write']:
            self.assertIn(permission, job)
        self.assertIn('"--latest=false"', text)
        self.assertLess(text.index('attest-build-provenance'), text.index('release", "create"'))
        self.assertIn('--changelog "$env:RELEASE_PRODUCT/CHANGELOG.md"', text)

    def test_every_product_is_packaged_only_when_present(self):
        self.assertEqual(self.products, ['bridge-events', 'bridge-files'])
        text = (ROOT/'.github/workflows/build.yml').read_text()
        for product in self.products:
            with self.subTest(product=product):
                self.assertIn(f"if: hashFiles('{product}/CMakeLists.txt') != ''", text)
                self.assertIn(f'ci-package-product.ps1 -Product {product} ', text)
                self.assertIn(f"if: steps.{product}.outcome == 'success'", text)
                self.assertIn(f'name: ${{{{ steps.{product}.outputs.artifact }}}}', text)
        self.assertTrue((ROOT/'bridge-events/CMakeLists.txt').is_file())
        self.assertTrue((ROOT/'bridge-events/CHANGELOG.md').is_file())
        self.assertTrue((ROOT/'bridge-events/mod.json').is_file())


if __name__ == '__main__':
    unittest.main()
