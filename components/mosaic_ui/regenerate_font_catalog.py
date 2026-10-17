#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Regenerate all Mosaic app bundles and their shared font catalog."""

import argparse
import os
from pathlib import Path
import subprocess
import sys

from tools.generate_bundle import UI_ROOT, build_scene, resolve_gspc


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gspc", type=Path, default=os.environ.get("GSPC"))
    parser.add_argument("--gsp-root", type=Path, default=os.environ.get("ESP_GSP_ROOT", os.environ.get("GSP_ROOT")))
    parser.add_argument("--profile", type=Path, default=os.environ.get("MOSAIC_SCENE_PROFILE"))
    args = parser.parse_args()
    if args.gspc is None:
        parser.error("pass --gspc or set GSPC to the GSPC executable")
    gspc = resolve_gspc(args.gspc)
    modules = [UI_ROOT / "hub", *(p.parent for p in sorted((UI_ROOT / "apps").glob("*/app.cmake")))]
    generated = UI_ROOT / "common/generated"
    linked = generated / "linked"
    try:
        bundles = [build_scene(module, gspc, module / "generated",
                               profile=args.profile, gsp_root=args.gsp_root) for module in modules]
        linked.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(gspc), "font-link", *(str(p) for p in bundles),
                        "--output-dir", str(linked), "--catalog", str(generated / "common-fonts.gspb"),
                        "--report", str(generated / "font-link-report.md")], check=True)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"mosaic: font catalog generation failed: {exc}", file=sys.stderr)
        return 1
    print(f"font catalog: {generated / 'common-fonts.gspb'}")
    print(f"font report: {generated / 'font-link-report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
