# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Host-side orchestration and actionable dependency diagnostics."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("generate_bundle", TOOLS / "generate_bundle.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class GenerationTests(unittest.TestCase):
    def test_native_arguments_and_asset_scene_resources(self):
        with tempfile.TemporaryDirectory(prefix="mosaic tests ") as directory:
            root = Path(directory)
            module = root / "settings"
            (module / "scene").mkdir(parents=True)
            output = root / "output with spaces"
            gspc = root / "compiler with spaces/gspc.exe"
            calls = []
            scratch = []

            def run(argv, **kwargs):
                calls.append((argv, kwargs))
                self.assertTrue(kwargs["check"])
                self.assertEqual(kwargs["cwd"], module / "scene")
                self.assertEqual(kwargs["env"]["MOSAIC_GENERATED_DIR"], str(output))
                if argv[0] == sys.executable:
                    return
                self.assertEqual(argv[0], str(gspc))
                if argv[1] == "build":
                    out = Path(argv[argv.index("-o") + 1])
                    scratch.append(out)
                    stem = Path(argv[2]).stem.removesuffix("_480")
                    for suffix in (".gsb", ".grb", "_font0.gfb", "_binds.h",
                                   "_actions.h", "_objects.h", "_templates.h"):
                        (out / (stem + suffix)).write_bytes(b"generated")
                elif argv[1] == "bundle":
                    # Settings must bundle the asset scene's GRB but keep the
                    # primary scene's fonts, headers, and scene ID.
                    members = [Path(p).name for p in argv[4:]]
                    self.assertEqual(members, ["settings.gsb", "settings_font0.gfb", "settings_assets.grb"])
                    Path(argv[3]).write_bytes(b"bundle")

            with patch.object(builder.subprocess, "run", side_effect=run):
                bundle = builder.build_scene(module, gspc, output)
            self.assertEqual(bundle.read_bytes(), b"bundle")
            self.assertEqual((output / "settings_templates.h").read_bytes(), b"generated")
            self.assertEqual([Path(argv[1]).name for argv, _ in calls[:2]],
                             ["gen_scene.py", "generate_root_icon_data.py"])
            self.assertEqual([argv[argv.index("--scene-id") + 1] for argv, _ in calls if argv[1] == "build"],
                             ["0", "1"])
            self.assertTrue(all(not path.exists() for path in scratch))

    def test_failure_stops_packaging_and_cleans_temporary_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calls = []
            scratch = []

            def run(argv, **kwargs):
                calls.append(argv)
                if argv[1] == "build":
                    scratch.append(Path(argv[argv.index("-o") + 1]))
                    raise subprocess.CalledProcessError(23, argv)

            with patch.object(builder.subprocess, "run", side_effect=run):
                with self.assertRaises(subprocess.CalledProcessError):
                    builder.build_scene(root / "music", root / "gspc", root / "out")
            self.assertFalse(any(argv[1] == "bundle" for argv in calls))
            self.assertFalse(scratch[0].exists())

    def test_fixed_and_overridden_profiles(self):
        override = Path("custom profile.yaml").resolve()
        self.assertEqual(builder.scene_profile(Path("apps/music"), override), override)
        self.assertEqual(builder.scene_profile(Path("apps/album"), override),
                         builder.UI_ROOT / "common/mosaic_rgb565_auto.yaml")
        self.assertEqual(builder.scene_profile(Path("apps/welcome"), override),
                         Path("apps/welcome/scene/esp32s31_rgb565_compact.yaml"))


class DependencyDiagnosticsTests(unittest.TestCase):
    def run_downloader(self, package_source):
        with tempfile.TemporaryDirectory(prefix="mosaic python ") as directory:
            root = Path(directory)
            package = root / "gsp"
            package.mkdir()
            (package / "__init__.py").write_text("")
            if package_source is not None:
                (package / "execute.py").write_text(package_source)
            env = dict(os.environ, PYTHONPATH=str(root))
            return subprocess.run(
                [sys.executable, "-S", str(TOOLS / "download_gspc.py"), "--component-dir", str(root)],
                env=env, capture_output=True, text=True,
            )

    def test_missing_manager_prints_install_command(self):
        result = self.run_downloader(None)
        self.assertEqual(result.returncode, 1)
        self.assertIn("-m pip install -U esp-gsp-tools", result.stderr)
        self.assertIn(sys.executable, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_old_manager_api_prints_upgrade_command(self):
        result = self.run_downloader("# Old manager without executable_from_environment\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unavailable or incompatible", result.stderr)
        self.assertIn("-m pip install -U esp-gsp-tools", result.stderr)

    def test_missing_version_is_not_reported_as_missing_manager(self):
        result = self.run_downloader("def executable_from_environment(*args, **kwargs): pass\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn(".gspc_version", result.stderr)
        self.assertNotIn("pip install", result.stderr)


if __name__ == "__main__":
    unittest.main()
