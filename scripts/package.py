#!/usr/bin/env python3
"""Package only clean source matching the full release gate. Never flashes a device."""

import json
import shutil
import subprocess
import zipfile

from release import ENVIRONMENTS, IMAGES, ROOT, digest, verify


def main():
    verification = verify()
    version = verification["version"]
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    manifest = {
        "version": version,
        "sourceCommit": commit,
        "sourceDigest": verification["sourceDigest"],
        "hardwareValidated": False,
        "gpioDefaults": "disabled",
        "completeFlashBundle": False,
        "images": [],
    }
    artifacts = ROOT / "artifacts"
    artifacts.mkdir(exist_ok=True)
    packaged = []
    for environment in ENVIRONMENTS:
        destination = artifacts / environment
        destination.mkdir(exist_ok=True)
        for name in IMAGES:
            source = ROOT / ".pio/build" / environment / name
            target = destination / name
            shutil.copy2(source, target)
            if digest(source) != digest(target):
                raise ValueError(f"Copy verification failed: {target}")
            packaged.append(target)
            manifest["images"].append(
                {
                    "file": str(target.relative_to(ROOT)),
                    "size": source.stat().st_size,
                    "kind": "application" if name == "firmware.bin" else "flash-support",
                    "sha256": digest(target),
                }
            )
    manifest_path = artifacts / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    readme = artifacts / "README.txt"
    readme.write_text(
        "ESP32-S3 compile-ready images; physical hardware NOT validated. GPIOs disabled.\n"
        "firmware.bin is the application image, NOT a standalone merged flash image.\n"
        "This is NOT a complete flashing bundle: the selected Arduino uploader may require\n"
        "boot_app0.bin, exact offsets, board/flash/USB settings and qualified GPIO flags.\n"
        "Use BUILD_GUIDE: qualify the board/pins/axes, rebuild, then PlatformIO upload.\n"
        "Do not guess flashing offsets or upload the application image as a merged image.\n"
    )
    packaged += [manifest_path, readme]
    hashes = artifacts / "SHA256SUMS"
    hashes.write_text("".join(f"{digest(path)}  {path.relative_to(ROOT)}\n" for path in packaged))
    packaged.append(hashes)
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).decode().split("\0")
    archive = ROOT.parent / f"NodX-Adapt-v{version}-prehardware.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        for name in filter(None, tracked):
            out.write(ROOT / name, "nodx-adapt/" + name)
        for path in packaged:
            out.write(path, "nodx-adapt/" + str(path.relative_to(ROOT)))
    with zipfile.ZipFile(archive) as archive_file:
        if archive_file.testzip():
            raise ValueError("Archive CRC verification failed")
    archive.with_suffix(".zip.sha256").write_text(f"{digest(archive)}  {archive.name}\n")
    print(archive)
    print(f"SHA256 {digest(archive)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(str(error)) from error
