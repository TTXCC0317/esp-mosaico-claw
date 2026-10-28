#!/usr/bin/env python3
"""Exercise both applications' actual NVS migrations with injected write failures."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CLAW = ROOT / "third-party/esp-claw"
HOST = CLAW / "components/claw_modules/claw_endpoint/test_host/include"

for standalone in (False, True):
    project = CLAW / "application/edge_agent" if standalone else ROOT
    shell = CLAW / "components/common/app_claw" if standalone else ROOT / "components/app_claw"
    with tempfile.TemporaryDirectory(prefix="endpoint-config-test-") as tmp:
        binary = Path(tmp) / "test_config"
        cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-DCONFIG_APP_WIFI_SSID=\"\"", "-DCONFIG_APP_WIFI_PASSWORD=\"\"",
               "-DCONFIG_APP_SEARCH_HTTP_ALLOWLIST=\"\"", "-include", str(HOST / "host_compat.h")]
        if not standalone:
            cmd += ["-DTEST_MOSAICO"]
        cmd += ["-I" + str(p) for p in [HERE / "include", HOST, project / "components/app_config/include", shell / "include"]]
        cmd += [str(HERE / "test_config.c"), str(project / "components/app_config/app_config.c"), "-o", str(binary)]
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)
