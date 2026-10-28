#!/usr/bin/env python3
"""Package an existing build as a validated Mosaico Ideas release ZIP."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
UTILS = ROOT / "third-party/esp-mosaico-utils"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, default=ROOT / "firmware_output/esp-mosaico-claw-release.zip")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    description = json.loads((build / "project_description.json").read_text())
    images = json.loads((build / "update_flasher_args.json").read_text())
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(UTILS / "mosaico-tools/tools"))
    from mosaico_cli.ideas_archive import package_project

    def run(script, *arguments):
        subprocess.run([sys.executable, str(script), *map(str, arguments)], check=True)

    # Only application resources enter the bundle; retained firmware stays untouched.
    with tempfile.TemporaryDirectory(prefix="ideas-") as temporary:
        stage = Path(temporary) / "firmware"
        bundle = Path(temporary) / "app.irisfw"
        print(f"Packaging existing build: {build}", flush=True)
        run(UTILS / "mosaico-tools/tools/prepare_system_update.py",
            "--partition-csv", ROOT / "partitions_16MB.csv",
            "--partition-table", build / "partition_table/partition-table.bin",
            "--application", build / images["app"]["file"],
            "--ui-apps", build / images["ui_apps"]["file"],
            "--data", f"system={build / images['system']['file']}",
            "--release", description["project_version"], "--stage-dir", stage)
        run(UTILS / "ESP-Iris/components/esp_iris/tools/esp_iris.py",
            "bundle", "build", stage / "manifest.json", "--component-root", stage, "--output", bundle)
        # The upstream packager validates firmware, bilingual assets and the final ZIP.
        result = package_project(ROOT, output, bundle)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Ideas packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
