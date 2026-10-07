#!/usr/bin/env python3
"""Bundle versioned source, compile-ready images and their hashes. No flashing."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / "VERSION").read_text().strip()
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
manifest = {"version": version, "sourceCommit": commit, "hardwareValidated": False,
            "gpioDefaults": "disabled", "images": []}
artifacts = ROOT / "artifacts"
artifacts.mkdir(exist_ok=True)
for environment in ("esp32s3", "esp32s3-sim"):
    destination = artifacts / environment
    destination.mkdir(exist_ok=True)
    for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
        source = ROOT / ".pio/build" / environment / name
        if not source.exists():
            raise SystemExit(f"Missing {source}; run pio run first")
        shutil.copy2(source, destination / name)
        manifest["images"].append({"file": str((destination / name).relative_to(ROOT)),
                                    "size": source.stat().st_size,
                                    "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
(artifacts / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(artifacts / "README.txt").write_text(
    "Compile-ready ESP32-S3 images; physical hardware NOT validated. GPIOs disabled by default.\n"
    "Use the source BUILD_GUIDE to qualify pins/axes, rebuild and upload with PlatformIO.\n"
    "firmware.bin is the application image, not a standalone merged flash image.\n")
tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
archive = ROOT.parent / f"NodX-Adapt-v{version}-prehardware.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
    for name in filter(None, tracked):
        out.write(ROOT / name, "nodx-adapt/" + name)
    for path in sorted(artifacts.rglob("*")):
        if path.is_file():
            out.write(path, "nodx-adapt/" + str(path.relative_to(ROOT)))
print(archive)
