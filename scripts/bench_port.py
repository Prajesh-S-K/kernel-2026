#!/usr/bin/env python3
"""Identify the bench board's native-USB port WITHOUT opening it (macOS).

Reads the USB registry (`ioreg`) for Espressif's built-in "USB JTAG/serial debug unit" (303a:1001),
lists /dev/cu.usbmodem* and asks `lsof` whether any process holds a port open. It never opens, resets
or writes to a port. The port name can change when the board resets or re-enumerates (for example after
an upload), so run this again after every upload and use the port it reports.

  python3 scripts/bench_port.py                  # report
  python3 scripts/bench_port.py --expect /dev/cu.usbmodem101   # exit 1 unless that is the one free port
"""

import argparse
import glob
import re
import subprocess
import sys

ESPRESSIF = (0x303A, 0x1001)


def parse_ioreg(text):
    """Return one dict per Espressif native-USB device found in `ioreg -p IOUSB -l -w0` text."""
    devices = []
    for block in re.split(r"\n(?=\s*\|?\s*\+-o )", text):
        vendor = re.search(r'"idVendor" = (\d+)', block)
        product = re.search(r'"idProduct" = (\d+)', block)
        if vendor and product and (int(vendor.group(1)), int(product.group(1))) == ESPRESSIF:
            name = re.search(r'"kUSBProductString" = "([^"]*)"', block)
            devices.append({"product": name.group(1) if name else "unknown"})
    return devices


def holders(lsof_text):
    """Process names listed by `lsof <port>` (empty when nothing holds it)."""
    lines = [line for line in lsof_text.splitlines() if line.strip()]
    return sorted({line.split()[0] for line in lines[1:]}) if len(lines) > 1 else []


def verdict(devices, ports, held, expect=None):
    """(ok, message). ok only when exactly one device and one port exist, unheld, and (if given) expected."""
    problems = []
    if len(devices) != 1:
        problems.append(f"expected exactly 1 Espressif native-USB device, found {len(devices)}")
    if len(ports) != 1:
        problems.append(f"expected exactly 1 /dev/cu.usbmodem* port, found {len(ports)}: {ports}")
    for port, names in held.items():
        if names:
            problems.append(f"{port} is held open by: {', '.join(names)}")
    if expect and ports != [expect]:
        problems.append(f"expected {expect}, found {ports}")
    if problems:
        return False, "NOT READY: " + "; ".join(problems)
    return True, f"READY: {ports[0]} is the only native-USB port and nothing holds it open"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--expect", help="the port you were told to expect")
    args = parser.parse_args(argv)
    registry = subprocess.run(
        ["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True
    ).stdout
    devices = parse_ioreg(registry)
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    held = {}
    for port in ports:
        held[port] = holders(subprocess.run(["lsof", port], capture_output=True, text=True).stdout)
    ok, message = verdict(devices, ports, held, args.expect)
    for device in devices:
        print(f'USB device: Espressif 303a:1001 "{device["product"]}"')
    for port in ports:
        print(f"port: {port}  holders: {held[port] or 'none'}")
    print(message)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
