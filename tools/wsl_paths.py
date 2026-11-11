"""Resolve host paths through the configured Ubuntu WSL distribution."""

from __future__ import annotations

import subprocess
from pathlib import Path


class WslPathError(RuntimeError):
    """Raised when the Ubuntu distribution cannot convert a host path."""


def to_wsl_path(path: Path) -> str:
    """Return the absolute Linux path that WSL assigns to ``path``."""
    command = [
        "wsl.exe", "-d", "Ubuntu", "--exec", "wslpath", "-a", "-u",
        str(path.resolve()),
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        raise WslPathError("Could not invoke wsl.exe to convert the host path.") from error

    if result.returncode != 0:
        detail = result.stderr.strip() or "no diagnostic output"
        raise WslPathError(
            f"wslpath conversion failed with exit code {result.returncode}: {detail}"
        )

    converted = result.stdout.strip()
    if not converted.startswith("/"):
        raise WslPathError("wslpath did not return an absolute Linux path.")
    return converted
