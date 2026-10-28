#!/usr/bin/env python3
"""Run the production trial provider with pthread-backed FreeRTOS and a fake issuer."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MODULES = ROOT / "third-party/esp-claw/components/claw_modules"
HOST = MODULES / "claw_endpoint/test_host/include"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"
includes = [HERE / "include", HERE.parent / "endpoint_config/include", HOST,
            ROOT / "components/trial_provider/include", ROOT / "components/trial_provider/src",
            ROOT / "components/region_service/include", CJSON]
includes += [MODULES / name / "include" for name in ("claw_endpoint", "claw_http", "claw_utils")]
sources = [HERE / "test_trial.c", *sorted((ROOT / "components/trial_provider/src").glob("*.c")),
           MODULES / "claw_endpoint/src/claw_endpoint.c", MODULES / "claw_endpoint/src/claw_endpoint_static.c",
           MODULES / "claw_utils/src/claw_utils_string.c", CJSON / "cJSON.c"]
with tempfile.TemporaryDirectory(prefix="trial-provider-test-") as tmp:
    binary = Path(tmp) / "test_trial"
    cmd = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra",
           "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-DCONFIG_TRIAL_DEVICE_HMAC_KEY_ID=-1", '-DCONFIG_TRIAL_SERVER_URL_CN="https://cn.test"',
           '-DCONFIG_TRIAL_SERVER_URL_INTL="https://intl.test"', "-include", str(HOST / "host_compat.h")]
    cmd += ["-I" + str(p) for p in includes]
    cmd += [str(p) for p in sources] + ["-pthread", "-lm", "-o", str(binary)]
    subprocess.run(cmd, check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
