# NodX desktop action overlay (EXPERIMENTAL, macOS only, optional)

A small floating **NodX ▾** tile that stays above your other windows, so the head-controlled pointer can pick an
action without going back to the browser palette. The browser palette stays available as a fallback; only one of
them controls the device at a time.

**Software-tested and attended-hardware results are separate.** Everything marked "tested" below ran in software
(unit tests, the simulated bridge, an off-screen render of the real panel). Nothing about the overlay has been
tried with the board and a person yet; the attended checklist is at the end.

## Set up and launch

Prerequisites: the companion running with the board (`python desktop/server.py --serial <port>`), and a session
you started yourself with "Start without calibration". The overlay never starts a session.

```bash
scripts/run_overlay.sh              # first run creates ./.venv-overlay and installs the one native dependency
scripts/run_overlay.sh --edge left  # first position on the left edge (default: right)
scripts/run_overlay.sh --selftest /tmp/nodx-overlay   # check the panel off screen and render every state to PNG
```

- The native dependency is `pyobjc-framework-Cocoa==12.2.2` in `overlay/requirements.txt`, installed into its own
  virtual environment. It is not a firmware or browser-companion requirement and the project environment is not
  touched. Tested with Python 3.14.6 on macOS 27.0.1 (arm64).
- Options: `--url` (the bridge, default `http://127.0.0.1:8791/api/device`), `--edge left|right`,
  `--keyboard-clicks macos|nodx` (see Keyboard).
- `NODX_OVERLAY_LOG=<file>` appends one JSON line per state change (for bench notes).
- Quit with Ctrl-C in the terminal. Closing the overlay switches its controls off on the device within 5 s.

## Permissions

The overlay needs **none** of the macOS privacy permissions: no Accessibility (AX) permission, no Screen
Recording, no Input Monitoring. It reads the pointer position with `NSEvent.mouseLocation` (no permission),
detects the keyboard with `NSWorkspace` running applications (no permission) and never posts input events.
The only setting you change yourself is the one-time Accessibility Keyboard switch described below.

## Walkthrough

1. Start the companion, then click **Start without calibration** in the browser. The tile appears near the right
   edge: **NodX ▾ · Dwell to start**. Pointing works, clicks are off.
2. Rest the pointer on the tile until its ring fills (the same dwell duration as everywhere, START 1.2 s). Passing
   over it does nothing. This claims the controls (an explicit act) and opens the menu:
   **Left-click, Right-click, Double-click, Drag, Drop, Scroll, Keyboard, Cancel, Pause / Stop**.
3. Rest on an item until its ring fills. The menu collapses and the tile shows the chosen action. The pointer
   must leave a control before it can be chosen again.
4. Move to your target and rest: the device clicks (Left-click is the default). Right-click and Double-click are
   one-shot unless "Keep selected action" is on in the browser panel. After any click, move deliberately before
   the next one can arm.
5. **Drag**: dwell on a target to press and hold (the tile turns red and shows **DRAGGING**), move, dwell again to
   release. **Drop** releases without any movement. **Cancel** releases and clears anything pending.
6. **Scroll**: dwell on the content; the pointer freezes and head movement scrolls. The tile opens a banner:
   **HOLD STILL TO EXIT SCROLLING** (pausing to read also exits). The pointer cannot travel while frozen, so use
   the website Stop or the physical button if you need an immediate stop.
7. **Pause / Stop** ends the session. The website Stop and the physical button always work too.
8. Move the tile with a real mouse or trackpad (drag it); it snaps to the nearest screen edge, stays fully inside
   the usable area and is remembered (`~/Library/Application Support/NodX Overlay/position.json`). The
   head-controlled pointer cannot move it (the device never presses on the overlay).

## Keyboard

What this macOS offers (checked on macOS 27.0.1): the Accessibility Keyboard is hosted by the system input method
`Assistive Control` and is started by launchd on demand. There is **no public API, app bundle, command-line tool
or documented URL that opens it**. The supported ways to show it are its switch in System Settings > Accessibility
> Keyboard and the Accessibility Shortcut. Driving those by UI scripting, coordinate clicks or posted key events
would be brittle and a second input path, so the overlay does not.

So **Keyboard** does this:
- If the Accessibility Keyboard's host process is running: NodX enters **keyboard mode**: the tile says
  **KEYBOARD**, pointing continues, and NodX's own target dwell clicks are paused so a key is never pressed
  twice (the keyboard supplies its own dwell). Choosing **any action** in the menu, or **Keyboard** again, returns
  to ordinary NodX control; so do Pause/Stop, a fault and a disconnect.
- Otherwise: it opens System Settings at the Accessibility pane (a supported deep link) and shows the one-time
  setup. It does not claim the keyboard opened and does not change any setting.

One-time setup (do it yourself): System Settings > Accessibility > Keyboard > **Accessibility Keyboard** on, and
turn on its **dwell** option so its keys are chosen by holding the pointer still. The exact wording of those
switches in your macOS version could not be checked without the interface; the deep link opens the Accessibility
pane. If you would rather have NodX's own dwell press the keys, start the overlay with `--keyboard-clicks nodx`
(then no clicks are paused and the keyboard's own dwell must stay off, or keys type twice).

Limits: detection only shows that the keyboard's host is running, not that its window is visible (reading window
lists would need the Screen Recording permission, which the overlay deliberately does not ask for).

## How it is built

- `overlay/logic.py` (pure): geometry, the dwell tracker and the state machine. `overlay/bridge.py`: HTTP client
  and a worker thread. `overlay/controller.py`: glue. `overlay/keyboard.py`: the keyboard helper. `overlay/app.py`:
  the AppKit panel (PyObjC). The first four have no native dependency and are tested everywhere.
- **One serial owner.** The overlay only sends HTTP requests to the existing companion bridge; it never opens a
  serial port. Every reply carries the device's full status, so its own commands double as status polls.
- **The firmware stays the single authority** for target dwell actions, the held-button state and the
  SafetyManager -> HIDManager -> BLE output. The overlay adds no mouse-injection path (a test fails if the overlay
  sources contain a serial or event-posting call).
- **One owner per dwell timer.** The overlay times only its own menu dwell and sends a validated
  `actions select <item>`; the device keeps timing target dwells. While the pointer is on the overlay the device
  consumes target dwells, and it never selects a menu item by itself (tested: a long rest on the menu selects
  nothing). The device refuses a selection unless the menu state is acknowledged, fresh, no drag is held and any
  release has been confirmed.
- **One controller at a time.** `actions overlay on` claims the palette; the browser palette's enable and hover
  commands are then refused, and the overlay's commands are refused while the browser palette is on.
- **Menu-active state (acknowledged).** The overlay reports "the pointer is on the overlay" (`actions menu
  open|close`) the moment the pointer enters or leaves (about 30 Hz sampling) and repeats it every 0.5 s. The
  device answers in its status (`actions.menu`, `actions.ready`). While active: target clicks are inhibited
  (pointing continues), a held drag is released and the release confirmed before any selection, and a pending
  click is cancelled. On close the dwell is cleared and deliberate movement is required before target actions
  re-arm.
- **Loss.** The overlay shows **LOST** and stops sending when the bridge does not answer for 2.5 s; the device
  stops acting on targets after 2.5 s without a report and switches the overlay controls OFF after 5 s, releasing a
  held button through its normal path. Nothing resumes by itself: after a loss, a device timeout, a stop, a fault
  or a reconnect the tile needs a new dwell on it (an explicit act); after a stop or a reboot a new session must be
  started again first.

## What is and is not claimed

- A click is delivered at the pointer position at the moment it is sent. The overlay window covers its own area
  and ignores mouse events without passing them on, so a click that lands while the pointer is on the overlay
  hits the overlay, not the application under it. That is the physical reason a pending target click cannot leak
  through the overlay, and it is **not** OS-wide click interception or a zero-click-through guarantee.
- Residual race, measured in the software tests: if the pointer enters the overlay AFTER a target dwell has
  completed and the entry report reaches the device AFTER the 150 ms commit wait, the click is already out. With
  the pointer on the overlay it lands on the overlay; if it had already moved on it lands where the pointer was.
  The browser-palette measurements (median report delay about 100 ms, worst 835 ms, stalls of seconds) apply to
  this link too: it is the same serial path.
- Not verified: what the Mac does with a button held when the overlay process crashes or Bluetooth drops. Native
  cleanup cannot guarantee a release in those cases. The device's own release paths (Drop, Cancel, Stop, the
  physical button, a dwell release, the 5 s overlay timeout and failed-release inhibition) stay in force. Keep the
  Bluetooth held-button link-loss test separate and attended.
- Display scaling and several displays: all geometry is in points, so Retina scaling does not enter it. Tested in
  software with synthetic layouts (negative origins, mixed sizes, a small screen); this Mac has one display
  (1440 x 900 points at 2x). Real multi-display behaviour is unverified.
- Full-screen and protected screens (limitations, to be confirmed in the attended test): the panel floats at the
  status-window level, follows all Spaces and may sit above full-screen apps, but it is **not** shown over the
  lock screen, the login window or secure system prompts, and exclusive full-screen apps or games may hide it.
  Content protection (DRM video) is irrelevant to a window drawn on top, but a window the system refuses to
  layer over is outside this overlay's control. While the pointer is on the overlay the head-controlled pointer
  cannot act on whatever it covers, by design.
- It is a prototype: START values, no persistence of selections, and the browser companion must be running.

## Attended hardware checklist (not done yet; needs the firmware with the overlay commands flashed)

1. Pointing with clicks off; website Stop; physical stop.
2. Tile appears only in your explicit session, near the edge; passing over it does nothing; a dwell opens the menu.
3. Every item: Left, Right (one shot), Double, Drag + Drop, Drag + Cancel, Scroll + stillness exit, Pause/Stop;
   keep mode; one selection per hover; collapse after selection.
4. Cursor entry near a pending target click (rest on a target, then move onto the tile just as the dwell ends).
5. Kill the overlay process during a drag, and unplug nothing: the device must release within 5 s.
6. Overlay Stop, website Stop and the physical button.
7. Applications: a text editor, Finder, a browser page; full-screen app; a second display if you have one.
8. Keyboard: with the Accessibility Keyboard on and its dwell enabled, open it from the menu, choose keys by
   head movement with no duplicate characters, then return to pointing by choosing an action.
9. Record the report delay (the overlay's `NODX_OVERLAY_LOG` plus the recorder), missed or accidental actions and
   failed releases; stop if reports go stale.
