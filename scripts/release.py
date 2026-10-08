"""Local release verification. Source and every built image must match the gate stamp."""

import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENTS = ("esp32s3", "esp32s3-sim")
IMAGES = ("firmware.bin", "bootloader.bin", "partitions.bin")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(root):
    names = (
        subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
        )
        .decode()
        .split("\0")
    )
    hasher = hashlib.sha256()
    for name in sorted(set(filter(None, names))):
        path = root / name
        if path.is_file():
            hasher.update(name.encode() + b"\0" + path.read_bytes() + b"\0")
    return hasher.hexdigest()


def artifact_paths():
    return [
        "build/nodx_sim",
        "build/nodx_tests",
        "build/nodx_handsfree_tests",
        "build/nodx_firmware_sim_tests",
        "build/nodx_firmware_hw_tests",
        "build/nodx_diag_tests",
    ] + [f".pio/build/{environment}/{image}" for environment in ENVIRONMENTS for image in IMAGES]


def stamp(root=ROOT):
    result = {
        "version": (root / "VERSION").read_text().strip(),
        "sourceDigest": source_digest(root),
        "gate": ["format", "lint", "native-ubsan", "python", "javascript", *ENVIRONMENTS],
        "artifacts": {name: digest(root / name) for name in artifact_paths()},
    }
    (root / "build/verification.json").write_text(json.dumps(result, indent=2) + "\n")


def verify(root=ROOT):
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"], cwd=root, text=True
    ).strip()
    if dirty:
        raise ValueError("Source is dirty; commit the verified source before packaging")
    result = json.loads((root / "build/verification.json").read_text())
    if result["version"] != (root / "VERSION").read_text().strip():
        raise ValueError("Release version differs from verification stamp")
    if result["sourceDigest"] != source_digest(root):
        raise ValueError("Source changed after verification; rerun the release gate")
    for name in artifact_paths():
        if digest(root / name) != result["artifacts"].get(name):
            raise ValueError(f"Stale or changed build artifact: {name}")
    return result


if __name__ == "__main__":
    stamp()
