"""Validate compatibility release metadata without changing vendor inputs."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def validate_release(manifest, header, cmake):
    values = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(r"^#define\s+COMPAT_VERSION_" + field + r"\s+(\d+)u?\s*$", header, re.M)
        if not match:
            raise ValueError("Missing firmware release field: " + field)
        values.append(int(match.group(1)))
    if not (0 <= values[0] <= 255 and all(0 <= v <= 15 for v in values[1:])):
        raise ValueError("Firmware version is outside the packed version format")
    version = ".".join(str(v) for v in values)
    if manifest["compatibilityVersion"] != version:
        raise ValueError("Release manifest and firmware version header differ")
    string = re.search(r'^#define\s+COMPAT_VERSION_STRING\s+"([0-9.]+)"\s*$', header, re.M)
    project = re.search(r"project\(\s*dp104_openrgb\s+VERSION\s+([0-9.]+)", cmake)
    if not string or not project or string.group(1) != version or project.group(1) != version:
        raise ValueError("Project version and firmware release differ")
    packed = (values[0] << 8) | (values[1] << 4) | values[2]
    if int(str(manifest["packedFirmwareVersion"]), 0) != packed:
        raise ValueError("Master packed version and PLC application version differ")
    return manifest


def load_release():
    return validate_release(
        json.loads((ROOT / "firmware/compatibility.json").read_text(encoding="utf-8")),
        (ROOT / "include/dp104_version.h").read_text(encoding="utf-8"),
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
