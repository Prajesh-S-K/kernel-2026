"""Launch: python -m overlay [--url URL] [--edge left|right] [--keyboard-clicks macos|nodx] [--keyboard osk|apple] [--open-keyboard switch|settings] [--selftest [DIR]]"""

import argparse
import json
import sys

from .bridge import DEFAULT_URL


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="python -m overlay", description=__doc__)
    parser.add_argument(
        "--url", default=DEFAULT_URL, help="the local companion bridge (default: %(default)s)"
    )
    parser.add_argument("--edge", choices=("left", "right"), default="right", help="first position")
    parser.add_argument(
        "--keyboard-clicks",
        choices=("macos", "nodx"),
        default="macos",
        help="who clicks the Accessibility Keyboard's keys: the macOS dwell (NodX target clicks paused while "
        "the keyboard is open, the default) or NodX's own dwell clicks",
    )
    parser.add_argument(
        "--keyboard",
        choices=("osk", "apple"),
        default="osk",
        help="what the Keyboard item shows: the NodX keyboard (NodX dwell presses the keys; needs the macOS "
        "permission to post key events) or the macOS Accessibility Keyboard",
    )
    parser.add_argument(
        "--open-keyboard",
        choices=("switch", "settings"),
        default="switch",
        help="what the Keyboard item does when the Accessibility Keyboard is not running: ask macOS to switch it "
        "on (its own on/off preference; undocumented, checked, falls back to Settings) or only open Settings",
    )
    parser.add_argument(
        "--selftest",
        nargs="?",
        const="",
        metavar="PNG_DIR",
        help="build the panel off screen, print its safety properties, optionally render every state to PNG files",
    )
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        print("The NodX overlay is macOS only.", file=sys.stderr)
        return 2
    try:
        from . import app
    except ImportError as error:
        print(
            "The overlay needs PyObjC: run scripts/run_overlay.sh (it makes a separate environment), or "
            "pip install -r overlay/requirements.txt.\n"
            f"({error})",
            file=sys.stderr,
        )
        return 2
    if args.selftest is not None:
        print(json.dumps(app.selftest(args.selftest or None), indent=2))
        return 0
    app.run(args.url, args.edge, args.keyboard_clicks, args.open_keyboard, args.keyboard)
    return 0


if __name__ == "__main__":
    sys.exit(main())
