"""The native Linux launcher selects one Python for the UI and child builds; Nix stays opt-in. No game files or GUI needed."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('nsp_native', ROOT / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux launcher')
class NativeLauncher(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nxasahi-native-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.repo = self.base / 'source checkout'
        self.launcher = self.repo / 'frontend/nxasahi.sh'
        self.launcher.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / 'frontend/nxasahi.sh', self.launcher)
        self.home = self.base / 'home'
        self.home.mkdir()
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), PATH=str(self.bin) + os.pathsep + os.defpath)
        for key in ('VIRTUAL_ENV', 'NXASAHI_PYTHON', 'NXASAHI_LAUNCHER'):
            self.env.pop(key, None)
        self.system = self.executable(self.bin / 'python3')
        self.executable(self.bin / 'nix', '#!/bin/sh\necho "unexpected Nix invocation" >&2\nexit 99\n')

    def executable(self, path, text=None):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text or '#!/bin/sh\nprintf "%s\\n" "$0" "$PWD" "$PATH" "${NXASAHI_LAUNCHER:-}" "$@"\n')
        path.chmod(0o755)
        return path

    def run_launcher(self, *args, ok=True):
        p = subprocess.run(['/bin/bash', str(self.launcher), *args], env=self.env, cwd=self.base, capture_output=True, text=True)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.splitlines()[1], str(self.repo))
        return p

    def test_checkout_venv_and_arguments(self):
        py = self.executable(self.repo / '.venv/bin/python3')
        p = self.run_launcher('a game.nsz', '--literal=value')
        lines = p.stdout.splitlines()
        self.assertEqual(lines[0], str(py))
        self.assertEqual(lines[2].split(os.pathsep)[:2], [str(py.parent), str(self.home / '.local/bin')])
        self.assertEqual(lines[3], str(self.launcher))
        self.assertEqual(lines[4:], ['frontend/nxasahi.py', 'a game.nsz', '--literal=value'])

    def test_active_environment_precedes_checkout(self):
        self.executable(self.repo / '.venv/bin/python3')
        active = self.base / 'active venv'
        py = self.executable(active / 'bin/python3')
        self.env['VIRTUAL_ENV'] = str(active)
        self.assertEqual(self.run_launcher().stdout.splitlines()[0], str(py))

    def test_explicit_python_precedes_active_environment(self):
        py = self.executable(self.base / 'custom python/bin/python3')
        self.env.update(NXASAHI_PYTHON=str(py), VIRTUAL_ENV='/not/used')
        self.assertEqual(self.run_launcher().stdout.splitlines()[0], str(py))

    def test_system_python_fallback(self):
        self.assertEqual(self.run_launcher().stdout.splitlines()[0], str(self.system))

    def test_missing_explicit_python_is_not_silently_replaced(self):
        self.env['NXASAHI_PYTHON'] = str(self.base / 'missing/python3')
        p = self.run_launcher(ok=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn('Python not found', p.stderr)

    def test_existing_launcher_override_is_preserved(self):
        self.env['NXASAHI_LAUNCHER'] = '/custom/launcher'
        self.assertEqual(self.run_launcher().stdout.splitlines()[3], '/custom/launcher')

    def test_nix_is_explicit_and_consumes_its_flag(self):
        nix = self.executable(self.bin / 'nix')
        self.env['NXASAHI_PYTHON'] = '/not/used'
        lines = self.run_launcher('--nix', 'a game.nsp').stdout.splitlines()
        self.assertEqual(lines[0], str(nix))
        self.assertEqual(lines[4:], ['develop', '--command', 'python3', 'frontend/nxasahi.py', 'a game.nsp'])


class NativePrerequisites(unittest.TestCase):
    def setUp(self):
        for p in (patch.object(nsp, '_importable', return_value=True), patch.object(nsp, 'linux_font_dir', return_value=Path('/fonts'))):
            p.start(); self.addCleanup(p.stop)
        self.xlate = ROOT / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'

    def test_no_nix_is_needed(self):
        with patch.object(nsp.shutil, 'which', side_effect=lambda tool, **kw: None if tool == 'nix' else '/bin/' + tool):
            nsp.check_prereqs_linux({'PATH': '/bin'}, self.xlate)

    def test_missing_build_tools_are_reported_before_import(self):
        for missing in ('ld.lld', 'ar', 'spirv-cross', 'glslangValidator'):
            with self.subTest(tool=missing), patch.object(nsp.shutil, 'which', side_effect=lambda tool, **kw: None if tool == missing else '/bin/' + tool):
                with self.assertRaises(nsp.BuildError) as e:
                    nsp.check_prereqs_linux({'PATH': '/bin'}, self.xlate)
                self.assertIn(missing, str(e.exception))
                self.assertIn('native dependencies', str(e.exception))


if __name__ == '__main__':
    unittest.main()
