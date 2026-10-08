"""Tests of the bench log/analysis tools on synthetic serial logs. No port is ever opened and no
hardware is involved: they show the tools read and judge text as documented, nothing about a board."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ANALYZE = load("bench_analyze")
LOG = load("bench_log")
PORT = load("bench_port")

INFO = """DIAG,info,chip=ESP32-S3,revision=2,cores=2,cpuMHz=240
DIAG,info,flashBytes=16777216,flashMHz=80,flashMode=2,psramBytes=8388608,freeHeap=300000
DIAG,info,resetReason=1,sdk=v5,arduinoEsp32=2.0.17
DIAG,info,usbCdcOnBoot=1
"""
SCAN = """DIAG,scan,idleSda=1,idleScl=1
DIAG,scan,address=0x68
DIAG,scan,found=1
"""
IMU = """DIAG,imu,whoAmIRead=1,whoAmI=0x68,expected=0x68
DIAG,imu,firmwareInitSequenceOk=1,dlpf=3,sampleDiv=9,gyroRange=250,accelRange=2
DIAG,imu,intEnableBefore=0x00
DIAG,imu,pollMs=1000,polls=900,dataReadyBitSeen=95
DIAG,imu,pollMs=1000,polls=900,dataReadyBitSeen=97
DIAG,imu,dataReadyNoIntEnable=95,dataReadyWithIntEnable=97
DIAG,imu,seconds=10,frames=1000,framesChanged=990,i2cErrors=0
DIAG,imu,intervalUsMean=10000,intervalUsMin=9800,intervalUsMax=11200
DIAG,imu,gyroMeanDps=0.4/-0.2/0.1,gyroStdDps=0.1/0.1/0.1
DIAG,imu,accelMeanG=0.01/0.02/0.99,accelStdG=0.01/0.01/0.01,accelMagMeanG=0.9902
DIAG,imu,tempC=27.10
"""
BUTTON = """DIAG,button,idleLevel=1,expected=1,note=pressed-reads-0
DIAG,button,prompt=press-and-release-the-button-now,seconds=20
DIAG,button,gateToggle=1,latched=1,atMs=5000
DIAG,button,burst=1,kind=press,edges=3,spanUs=400
DIAG,button,burst=2,kind=release,edges=5,spanUs=900
DIAG,button,edges=8,droppedEdges=0,bursts=2,gateToggles=1,latchedAtEnd=1
"""
BLE = """BLE,state,why=link-change,atMs=100,secured=0,subscribed=0,connected=0,buttonDown=0
BLE,state,why=link-change,atMs=900,secured=1,subscribed=1,connected=1,buttonDown=0
BLE,send,what=ping,delivered=1,sentOk=1,sentFailed=0
"""
GOOD = INFO + SCAN + IMU + BUTTON + BLE


def status_of(text, name_part):
    for item in ANALYZE.analyze_text(text)["checks"]:
        if name_part in item["name"]:
            return item["status"]
    raise AssertionError(f"no check named like {name_part!r}")


class Analysis(unittest.TestCase):
    def test_a_good_synthetic_log_has_no_failures(self):
        result = ANALYZE.analyze_text(GOOD)
        self.assertFalse(
            [c for c in result["checks"] if c["status"] in ("FAIL", "MISSING")], result["report"]
        )
        self.assertIn("not 'validated'", result["report"])

    def test_missing_sections_are_reported_not_skipped(self):
        result = ANALYZE.analyze_text(INFO)
        missing = {c["name"] for c in result["checks"] if c["status"] == "MISSING"}
        self.assertEqual(missing, {"I2C scan", "MPU6050 test", "enable button test", "BLE probe"})

    def test_data_ready_only_with_int_enable_is_a_failure_with_the_reason(self):
        bad = GOOD.replace("dataReadyNoIntEnable=95", "dataReadyNoIntEnable=0")
        self.assertEqual(status_of(bad, "data-ready gating"), "FAIL")
        item = [c for c in ANALYZE.analyze_text(bad)["checks"] if "data-ready" in c["name"]][0]
        self.assertIn("INT_ENABLE", item["detail"])
        never = bad.replace("dataReadyWithIntEnable=97", "dataReadyWithIntEnable=0")
        self.assertEqual(status_of(never, "data-ready gating"), "FAIL")

    def test_each_start_threshold_can_fail(self):
        cases = {
            "idle HIGH": GOOD.replace("idleSda=1", "idleSda=0"),
            "answers at 0x68": GOOD.replace("address=0x68", "address=0x69"),
            "WHO_AM_I": GOOD.replace("whoAmI=0x68,expected", "whoAmI=0x70,expected"),
            "no I2C read errors": GOOD.replace("i2cErrors=0", "i2cErrors=3"),
            "frames about 100": GOOD.replace("framesChanged=990", "framesChanged=400"),
            "gyro bias": GOOD.replace("gyroMeanDps=0.4/", "gyroMeanDps=9.0/"),
            "accelerometer magnitude": GOOD.replace("accelMagMeanG=0.9902", "accelMagMeanG=1.4"),
            "worst 10 ms": GOOD.replace("intervalUsMax=11200", "intervalUsMax=80000"),
            "flash size": GOOD.replace("flashBytes=16777216", "flashBytes=8388608"),
            "PSRAM": GOOD.replace("psramBytes=8388608", "psramBytes=0"),
            "PSRAM is": GOOD.replace("psramBytes=8388608", "psramBytes=4194304"),
            "bounce stays": GOOD.replace("edges=5,spanUs=900", "edges=60,spanUs=900"),
            "pin idles HIGH": GOOD.replace("idleLevel=1", "idleLevel=0"),
            "edges dropped": GOOD.replace("droppedEdges=0", "droppedEdges=4"),
            "secured, subscribed link": GOOD.replace(
                "secured=1,subscribed=1", "secured=1,subscribed=0"
            ),
            "commanded reports delivered": GOOD.replace("delivered=1", "delivered=0"),
        }
        for name, text in cases.items():
            with self.subTest(name):
                self.assertEqual(status_of(text, name), "FAIL")
                self.assertEqual(status_of(GOOD, name), "PASS")

    def test_psram_as_reported_by_a_real_board_counts_as_8_mib(self):
        # The heap-reported size is a little under 8 MiB (measured on the bench: 8386295 bytes).
        real = GOOD.replace("psramBytes=8388608", "psramBytes=8386295")
        self.assertEqual(status_of(real, "PSRAM is"), "PASS")

    def test_register_dump_is_recorded_and_frozen_frames_are_flagged(self):
        live = "".join(
            f"DIAG,regs,frame={i},read=1,raw=0000{i:02X}0000000000000000000000\n" for i in range(3)
        )
        frozen = "".join(
            "DIAG,regs,frame=%d,read=1,raw=00000000000000000000000000\n" % i for i in range(3)
        )
        head = "DIAG,regs,reg=PWR_MGMT_1,address=0x6B,read=1,value=0x01\n"
        self.assertEqual(status_of(GOOD + head + live, "raw frames identical"), "PASS")
        self.assertEqual(status_of(GOOD + head + frozen, "raw frames identical"), "FAIL")
        self.assertIn(
            "PWR_MGMT_1=0x01",
            [
                c
                for c in ANALYZE.analyze_text(GOOD + head + live)["checks"]
                if "registers" in c["name"]
            ][0]["detail"],
        )

    def test_tilt_test_is_judged_from_the_ranges_each_channel_covered(self):
        summary = "DIAG,raw,summary=1,seconds=20,frames=900,distinctFrames=900,i2cErrors=0\n"
        live = "DIAG,raw,rangeAccelLsb=15000/9000/17000,rangeGyroLsb=2000/1500/900,rangeTempLsb=3\n"
        dead = "DIAG,raw,rangeAccelLsb=2/1/3,rangeGyroLsb=0/0/0,rangeTempLsb=1\n"
        self.assertEqual(status_of(GOOD + summary + live, "raw accel responds"), "PASS")
        self.assertEqual(status_of(GOOD + summary + live, "raw gyro responds"), "PASS")
        self.assertEqual(status_of(GOOD + summary + dead, "raw accel responds"), "FAIL")
        self.assertEqual(status_of(GOOD + summary + dead, "raw gyro responds"), "FAIL")
        # The 4 Hz sample lines are not summaries and must not be mistaken for one.
        sample = "DIAG,raw,ax=1,ay=2,az=3,gx=4,gy=5,gz=6,temp=7\n"
        self.assertNotIn(
            "raw accel responds", [c["name"] for c in ANALYZE.analyze_text(GOOD + sample)["checks"]]
        )

    def test_garbage_and_truncated_lines_do_not_crash(self):
        noisy = (
            "boot junk \x00\x01\nDIAG,imu,gyroMeanDps=a/b/c\nDIAG,\nBLE,state\nrst:0x1 (POWERON)\n"
            + GOOD
        )
        result = ANALYZE.analyze_text(noisy)
        self.assertGreater(result["ignoredLines"], 0)
        self.assertEqual(status_of(noisy, "MPU6050 answers"), "PASS")

    def test_link_loss_with_the_button_down_is_a_warning(self):
        text = GOOD + "BLE,warn,link-lost-with-button-down=host-side-release-must-be-checked\n"
        self.assertEqual(status_of(text, "link lost while the button was down"), "WARN")

    def test_timestamps_and_prefixes_from_the_logger_are_accepted(self):
        stamped = "\n".join(f"12:00:00.000000 {line}" for line in GOOD.splitlines())
        self.assertEqual(status_of(stamped, "MPU6050 answers"), "PASS")


class FakeLink:
    def __init__(self, script):
        self.script = list(script)
        self.written = []

    def read(self):
        return self.script.pop(0) if self.script else b""

    def write(self, data):
        self.written.append(data)


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds


class Logger(unittest.TestCase):
    def test_session_writes_timestamped_lines_and_sends_only_what_was_named(self):
        clock = Clock()
        link = FakeLink(
            [b"DIAG,scan,idleSda=1,idleScl=1\nDIAG,scan,addr", b"ess=0x68\nDIAG,scan,found=1\n"]
        )
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "session"
            text = LOG.run_session(link, ["scan", "imu 10"], 6, out, clock=clock, sleep=clock.sleep)
            raw = (out / "raw.log").read_text()
        self.assertEqual(link.written, [b"scan\n", b"imu 10\n"])
        self.assertIn("host sent: scan", raw)
        self.assertIn("DIAG,scan,address=0x68", text)
        self.assertEqual(status_of(text + INFO, "MPU6050 answers"), "PASS")

    def test_lines_can_be_echoed_live_for_the_operator(self):
        clock = Clock()
        shown = []
        link = FakeLink([b"DIAG,button,prompt=press-and-release-the-button-now,seconds=20\n"])
        with tempfile.TemporaryDirectory() as tmp:
            LOG.run_session(
                link, [], 2, Path(tmp) / "s", clock=clock, sleep=clock.sleep, echo=shown.append
            )
        self.assertEqual(shown, ["DIAG,button,prompt=press-and-release-the-button-now,seconds=20"])

    def test_only_short_diagnostic_commands_are_accepted(self):
        for ok in (
            "info",
            "scan",
            "imuregs",
            "imuraw",
            "imuraw 30",
            "imu 10",
            "button 20",
            "down confirm",
            "up",
            "nudge",
        ):
            self.assertTrue(LOG.SAFE_COMMAND.fullmatch(ok), ok)
        for bad in ("rm -rf /", "imu 1000", "flash", "ota", "imu;ls", "", "down  confirm"):
            self.assertFalse(LOG.SAFE_COMMAND.fullmatch(bad), bad)

    def test_a_port_is_never_guessed_and_analysis_needs_none(self):
        run = lambda *args: subprocess.run(  # noqa: E731
            [sys.executable, str(ROOT / "scripts" / "bench_log.py"), *args],
            capture_output=True,
            text=True,
        )
        refused = run("--label", "x")
        self.assertEqual(refused.returncode, 2)
        self.assertIn("--port is required", refused.stderr)
        refused = run("--port", "/dev/cu.usbmodem999", "--send", "rm -rf /")
        self.assertEqual(refused.returncode, 2)
        self.assertIn("refusing unexpected command", refused.stderr)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "raw.log"
            path.write_text(GOOD)
            analysed = run("--analyze", str(path))
        self.assertEqual(analysed.returncode, 0)
        self.assertIn("Totals:", analysed.stdout)

    def test_sessions_land_under_the_git_ignored_evidence_folder(self):
        target = LOG.session_dir("stage 1 / odd*name")
        self.assertEqual(target.parent, ROOT / "hardware-evidence")
        self.assertNotIn("/", target.name[15:])
        ignored = subprocess.run(["git", "check-ignore", "-q", str(target)], cwd=ROOT)
        self.assertEqual(ignored.returncode, 0, "hardware-evidence/ must be git-ignored")


IOREG_ONE = """+-o Root  <class IORegistryEntry, id 0x100000100>
  +-o USB JTAG/serial debug unit@00100000  <class IOUSBHostDevice, id 0x100032adc>
  |   {
  |     "kUSBProductString" = "USB JTAG/serial debug unit"
  |     "idVendor" = 12346
  |     "idProduct" = 4097
  |   }
  +-o Some Keyboard@00200000  <class IOUSBHostDevice, id 0x100032ade>
  |   {
  |     "kUSBProductString" = "Keyboard"
  |     "idVendor" = 1452
  |     "idProduct" = 800
  |   }
"""
LSOF_HELD = """COMMAND   PID USER   FD   TYPE DEVICE SIZE/OFF NODE NAME
screen   4242 me    5u   CHR    9,6      0t0  613 /dev/cu.usbmodem101
"""


class DiagnosticSource(unittest.TestCase):
    def test_no_identifier_collides_with_an_arduino_macro(self):
        # Arduino.h defines word(...) as makeWord(...). A lambda named `word` therefore silently
        # decoded every frame as makeWord(<byte offset>), which made the first bench run report
        # frozen near-zero accel/gyro values that were really the offsets 0,2,4,6,8,10,12.
        source = (ROOT / "firmware" / "diag" / "diag_main.cpp").read_text()
        for macro in (
            "word",
            "bit",
            "min",
            "max",
            "abs",
            "round",
            "constrain",
            "radians",
            "degrees",
        ):
            self.assertNotRegex(source, rf"(?<![\w.:])auto\s+{macro}\s*=", macro)
            self.assertNotRegex(source, rf"(?<![\w.:>]){macro}\(\s*\d", f"{macro}(<number>) call")


class PortIdentification(unittest.TestCase):
    def test_only_the_espressif_native_usb_device_is_recognised(self):
        devices = PORT.parse_ioreg(IOREG_ONE)
        self.assertEqual(devices, [{"product": "USB JTAG/serial debug unit"}])
        self.assertEqual(PORT.parse_ioreg(IOREG_ONE.replace("12346", "1452")), [])
        self.assertEqual(PORT.parse_ioreg(""), [])

    def test_holders_are_the_process_names_not_the_header(self):
        self.assertEqual(PORT.holders(LSOF_HELD), ["screen"])
        self.assertEqual(PORT.holders(""), [])
        self.assertEqual(PORT.holders("COMMAND PID USER\n"), [])

    def test_ready_only_with_one_device_one_free_port_and_the_expected_name(self):
        device = [{"product": "x"}]
        port = "/dev/cu.usbmodem101"
        self.assertTrue(PORT.verdict(device, [port], {port: []}, port)[0])
        self.assertFalse(PORT.verdict([], [port], {port: []})[0])
        self.assertFalse(PORT.verdict(device * 2, [port], {port: []})[0])
        self.assertFalse(PORT.verdict(device, [], {})[0])
        self.assertFalse(PORT.verdict(device, [port, "/dev/cu.usbmodem2101"], {})[0])
        occupied = PORT.verdict(device, [port], {port: ["screen"]})
        self.assertFalse(occupied[0])
        self.assertIn("screen", occupied[1])
        moved = PORT.verdict(device, ["/dev/cu.usbmodem2101"], {"/dev/cu.usbmodem2101": []}, port)
        self.assertFalse(moved[0], "the port changed and must be re-identified")

    def test_only_plain_serial_device_names_are_accepted_as_a_port(self):
        for ok in ("/dev/cu.usbmodem101", "/dev/tty.usbmodem2101", "/dev/ttyACM0", "COM7"):
            self.assertTrue(LOG.PORT_NAME.fullmatch(ok), ok)
        for bad in (
            "/dev/disk0",
            "/etc/passwd",
            "../cu.x",
            "/dev/cu.",
            "usbmodem101",
            "/dev/cu.a b",
        ):
            self.assertFalse(LOG.PORT_NAME.fullmatch(bad), bad)
        refused = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "bench_log.py"), "--port", "/etc/passwd"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(refused.returncode, 2)
        self.assertIn("not a plain serial device name", refused.stderr)

    def test_the_logger_opens_the_port_with_dtr_and_rts_low(self):
        source = (ROOT / "scripts" / "bench_log.py").read_text()
        self.assertLess(source.index("self.port.dtr = False"), source.index("self.port.open()"))
        self.assertLess(source.index("self.port.rts = False"), source.index("self.port.open()"))


if __name__ == "__main__":
    unittest.main()
