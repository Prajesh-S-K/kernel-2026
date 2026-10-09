# NodX desktop action overlay (EXPERIMENTAL, macOS only, optional)

A small floating **NodX ▾** tile that stays above your other windows, so the head-controlled pointer can pick an
action (Left-click, Right-click, Double-click, Drag/Drop, Scroll, Keyboard, Cancel, Pause / Stop) in any
application without going back to the browser palette. It also provides a **NodX keyboard** for typing by
dwell. The browser palette stays available as a fallback; only one of them controls the device at a time.

The overlay is an optional, separate program: the firmware, the companion and the browser UI work without it.
It talks only to the existing local companion bridge and never opens a serial port.

**Status.** Software tests cover the logic, the real panel path and the stale-command protection (see
*Verification*). On the bench board the author attended: pointing, both stop controls, the tile, opening the
menu, selecting menu items (after the readiness fix below) and typing with the NodX keyboard were found
satisfactory. That attended use was informal, not a measured protocol; what is **not** verified is listed under
*Verification*. This is a prototype, not a certified assistive device.

## Requirements

- macOS (developed on macOS 27.0.1, arm64, Python 3.14.6), a Python 3 with `venv`, and the board with firmware
  that includes the overlay commands (`actions overlay|menu|select|keyboard`, status fields `controller`, `menu`,
  `ready`, `session`, `epoch`, `epochFloor`).
- The companion running against the board: `python desktop/server.py --serial <port>` (see the main README).
- One native dependency, `pyobjc-framework-Cocoa==12.2.2` (`overlay/requirements.txt`), installed into its own
  virtual environment by the launch script. The project environment is not touched.

## Quick start

```bash
# 1. companion (as usual)
python desktop/server.py --serial <port>

# 2. in the browser (http://127.0.0.1:8791) click "Start without calibration"  (the overlay never starts a session)

# 3. the overlay
scripts/run_overlay.sh              # first run creates ./.venv-overlay
```

The tile appears near the right edge: **NodX ▾ · Dwell to start**. Rest on it until its ring fills to open the
menu. Quit with Ctrl-C; closing the overlay switches its controls off on the device within 5 s.

Options (all optional):

| Option | Default | Meaning |
|---|---|---|
| `--url URL` | `http://127.0.0.1:8791/api/device` | the local companion bridge |
| `--edge left\|right` | `right` | first position of the tile (moved afterwards with a real mouse, remembered) |
| `--keyboard osk\|apple` | `osk` | the **NodX keyboard**, or the macOS Accessibility Keyboard |
| `--keyboard-clicks macos\|nodx` | `macos` | `apple` only: who clicks that keyboard's keys |
| `--open-keyboard switch\|settings` | `switch` | `apple` only: how the Keyboard item starts it |
| `--selftest [DIR]` | | build the panel off screen, print its safety properties, render every state to PNG |

Bench aids (environment variables): `NODX_OVERLAY_LOG=<file>` appends one JSON line per state change;
adding `NODX_OVERLAY_DIAG=1` also shows the hovered item, dwell progress, restart count, pointer displacement and
the reason a dwell is not progressing in a yellow line on the tile, and logs every dwell restart with its cause
and the device's `menu`/`ready` state. Use them when something does not respond.

## Permissions

- **Menu and clicking: none.** The overlay reads the pointer with `NSEvent.mouseLocation` (no permission) and
  posts no mouse events.
- **NodX keyboard typing: Accessibility, once.** macOS drops posted key events unless the app running the
  overlay, named **Python**, is allowed in System Settings > Privacy & Security > Accessibility (add it with the
  **+** button; the path is the `Python.app` inside the Python framework that `python3 --version` runs from. In
  the file dialog press Cmd+Shift+G to type a path). Until then the keyboard shows "typing not allowed", opens that
  pane once and sends nothing. **macOS may quit the overlay when you grant the permission: start it again.** If
  the permission seems ignored, the app that launched the overlay (for example your terminal) may need it too.
- Not needed: Screen Recording, Input Monitoring.

## Walkthrough

1. Start the companion, then click **Start without calibration**. Pointing works, clicks are off.
2. Rest on the tile until its ring fills (the dwell duration shown by the device, START 1.2 s). Passing over it
   does nothing. This claims the controls (an explicit act) and opens the menu:
   **Left-click, Right-click, Double-click, Drag, Drop, Scroll, Keyboard, Cancel, Pause / Stop**.
3. Rest on an item until its ring fills. The menu collapses and the tile shows the chosen action. The pointer
   must leave a control before it can be chosen again.
4. Move to your target and rest: the device clicks (Left-click is the default). Right-click and Double-click are
   one-shot unless "Keep selected action" is on in the browser panel. After any click, move deliberately before
   the next one can arm.
5. **Drag**: dwell on a target to press and hold (the tile turns red: **DRAGGING**), move, dwell again to release.
   **Drop** releases without movement. **Cancel** releases and clears anything pending.
6. **Scroll**: dwell on the content; the pointer freezes and head movement scrolls. A banner says **HOLD STILL TO
   EXIT SCROLLING** (pausing to read also exits). Use the website Stop or the physical button for an immediate stop.
7. **Pause / Stop** ends the session; a new one must be started again explicitly. The website Stop and the
   physical button always work too.
8. Move the tile by dragging it with a real mouse or trackpad; it snaps to the nearest screen edge, stays fully
   inside the usable area and is remembered (`~/Library/Application Support/NodX Overlay/position.json`). The
   head-controlled pointer cannot move it.

## The NodX keyboard (default)

**Keyboard** shows a keyboard window at the bottom of the display that holds the tile. Keys are chosen by the
overlay's own dwell (60 % of the device dwell time, never under 0.45 s); each key must be left before it types
again; **Shift** is one-shot; **Close**, **Keyboard** again, **Pause / Stop** or a lost link hides it. First click
where the text should go with a normal NodX click: the keyboard never takes focus.

- **One click generator.** The device does not act on target dwells while the pointer is over the overlay, and the
  keyboard window is part of the overlay (reported to the device like the tile and menu), so a key is never also
  clicked by NodX. The macOS Accessibility Keyboard and its dwell are not involved, and NodX clicking elsewhere is
  not paused. You do not need to switch any macOS dwell option on; leave them off.
- **Key events only.** Typing requests are posted from one place, `overlay/typing.py`, as key events (never mouse
  events), only while the overlay holds the controls, the link is fresh, the pointer is on a key and no Drag or
  Scroll is active. A test fails if the sources contain a mouse-event call or key posting anywhere else.
- **Layout.** QWERTY letters, digits, `- ' , . ? @`, space, Backspace, Return and Shift (digits give `!@#$%^&*()`).
  No key repeat, no modifiers other than Shift. Secure text fields may refuse posted key events: do not use it for
  passwords.

## The macOS Accessibility Keyboard (optional, `--keyboard apple`)

What macOS 27.0.1 offers: the Accessibility Keyboard is hosted by the input method `Assistive Control`; there is
no public API, command or documented URL that opens it. The **Keyboard** item then:

- If the keyboard's host process is running: enters **keyboard mode**: the tile says **KEYBOARD**, pointing
  continues and (with `--keyboard-clicks macos`) NodX's own target dwell clicks are paused so a key is never
  pressed twice. Any action, **Keyboard** again, Pause / Stop, a fault or a disconnect returns to NodX control. A
  running host process does **not** prove a keyboard is on screen; the overlay says so and keeps Cancel and Stop
  reachable.
- Otherwise, with `--open-keyboard switch`: asks macOS to switch it on by setting the same preference the Settings
  switch stores (`virtualKeyboardOnOff` in `com.apple.universalaccess`; only that, never a dwell option, never
  off). This is **undocumented and unverified** across macOS versions, so the overlay waits 4 s for the host
  process and otherwise opens System Settings at the Accessibility pane with the one-time setup. With
  `--open-keyboard settings` it only opens that pane.
- The tile's hint while keyboard mode lasts says: "Keyboard mode: NodX clicks are paused. NodX only knows the
  keyboard's host process is running, not that a keyboard is on screen. No keyboard? Dwell on this tile and choose
  Keyboard again, Cancel or any action to get NodX clicks back. Pause / Stop and the physical button also work."
  If you see no keyboard: (1) dwell on the NodX tile (the menu is timed by the overlay, so it works while NodX
  clicks are paused) and choose **Keyboard** again, **Cancel** or any action; (2) show the keyboard with the
  Accessibility Shortcut or System Settings > Accessibility > Keyboard; (3) **Pause / Stop**, the website Stop and
  the physical button are always reachable. If the host process quits while keyboard mode is on, the overlay turns
  keyboard mode off by itself and says so.
- Leaving keyboard mode does **not** switch off that keyboard's own dwell (or Dwell Control under Pointer
  Control): two click generators then give duplicate clicks. Choose ONE: NodX clicks (turn the keyboard's dwell
  off, `--keyboard-clicks nodx` if NodX should press its keys) or macOS dwell (stay in keyboard mode). The macOS
  dwell settings cannot be read or changed by NodX.
  Check once, attended: after choosing an action to leave keyboard mode, rest on a harmless target and confirm
  exactly one click happens. Unverified: whether the keyboard's own dwell still acts on other windows while its
  keyboard is hidden on your macOS version; assume it may, and turn it off if you see extra clicks.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| No tile appears | No session is active (the overlay shows only during a session you started), or the overlay quit: start it again. macOS can quit it when you grant Accessibility. |
| Menu opens but items show no progress | Run with `NODX_OVERLAY_DIAG=1`. "device not ready" means the status did not acknowledge the menu; "moved" restarts mean the pointer drifted more than 14 pt during the dwell. |
| Tile says LOST | The bridge did not answer for 2.5 s. Nothing resumes by itself: dwell on the tile again. If it repeats, check the companion and the serial link. |
| "The browser palette is active" | Only one controller at a time: switch the browser palette off. |
| Keyboard says "typing not allowed" | Grant Accessibility to **Python** (see Permissions); it starts typing within 2 s. |
| A character types twice | Two click generators or a key held in the target app; check the macOS dwell options are off. |

## Verification

**Software** (all run in CI-style local tests): the C++ core and firmware command tests (`tests/test_actions.cpp`,
`tests/test_firmware_commands.cpp`), the Python tests `tests/test_overlay_logic.py`, `test_overlay_bridge.py`,
`test_overlay_stale.py`, `test_overlay_osk.py`, `test_overlay_native.py` (the last needs the overlay environment:
`.venv-overlay/bin/python -m unittest discover -s tests -p "test_overlay*.py"`), and the bridge/protocol tests.
They cover geometry on synthetic display layouts, dwell rules, stale and queued commands after Stop/timeout/
reconnect, loss handling, keyboard recovery, the real panel and controller path (opening the tile, dwelling on an
item, dwelling on a key), and a source scan that forbids a serial port, mouse events and key posting outside
`overlay/typing.py`. Mutation checks confirmed the key safety tests fail when the guarded behaviour is removed.

**Attended on the bench board (informal):** pointing, the website Stop and the physical stop, the tile, opening the
menu, selecting items and NodX-keyboard typing were found working. A defect found in this attended run (menu items
never selected) was diagnosed from the diagnostics log: the device builds the reply to a command before its next
control tick, so every reply to a `menu` command said `menu=false ready=false` while plain `status` polls said
`true`. The overlay now takes `menu`/`ready` only from status polls and polls status every 0.2 s while the menu is
open; the firmware is unchanged and the selection rules (acknowledged, fresh, no held drag) still apply on the
device. Command round trips on the bench were 20-40 ms in that run; earlier link measurements (median 105 ms, 95 %
516 ms, worst 835 ms, stalls of seconds) still apply to the same serial path.

**Not verified:** the Bluetooth link-loss behaviour with a button held (keep that test separate and attended),
more than one display, full-screen apps, whether the macOS keyboard's own dwell acts while its keyboard is hidden,
real-application typing across many apps, and long-run reliability.

## How it is built

- `overlay/logic.py` (pure): geometry, the dwell tracker and the state machine. `overlay/bridge.py`: HTTP client
  and a worker thread. `overlay/controller.py`: glue. `overlay/keyboard.py`: the macOS Accessibility Keyboard
  helper. `overlay/osk.py` (pure): the NodX keyboard's layout and dwell typing. `overlay/typing.py`: key-event
  posting through ctypes. `overlay/app.py`: the AppKit panels (PyObjC). All but `typing.py` and `app.py` have no
  native dependency and are tested everywhere.
- **One serial owner.** The overlay only sends HTTP requests to the existing companion bridge; it never opens a
  serial port. Every reply carries the device's full status, so its own commands double as status polls.
- **The firmware stays the single authority** for target dwell actions, the held-button state and the
  SafetyManager -> HIDManager -> BLE output. The overlay adds no mouse-injection path (a test fails if the overlay
  sources contain a serial port or mouse-event call, or key posting outside `overlay/typing.py`).
- **One owner per dwell timer.** The overlay times only its own menu dwell and sends a validated
  `actions select <item>`; the device keeps timing target dwells. While the pointer is on the overlay the device
  consumes target dwells, and it never selects a menu item by itself (tested: a long rest on the menu selects
  nothing). The device refuses a selection unless the menu state is acknowledged, fresh, no drag is held and any
  release has been confirmed.
- **One controller at a time.** `actions overlay on` claims the palette; the browser palette's enable and hover
  commands are then refused, and the overlay's commands are refused while the browser palette is on.
- **Menu-active state (acknowledged).** The overlay reports "the pointer is on the overlay" (`actions menu
  open|close`) the moment the pointer enters or leaves (about 30 Hz sampling) and repeats it every 0.5 s. The
  device answers in its status (`actions.menu`, `actions.ready`), which the overlay reads from status polls only
  (the reply to the `menu` command itself is built before the device's next tick and always says not ready). While active: target clicks are inhibited
  (pointing continues), a held drag is released and the release confirmed before any selection, and a pending
  click is cancelled. On close the dwell is cleared and deliberate movement is required before target actions
  re-arm.
- **Stale commands never act.** A command or reply from an old overlay session must not reclaim the controls,
  reopen the menu or run an action after a Stop, a timeout or a reconnect (tested explicitly):
  the device keeps a **session serial** (every explicit start is a new session) and accepts an overlay claim only
  for the current session with an **epoch** higher than any it accepted before; menu, select and keyboard commands
  must carry the epoch of the claim in force, so an old one is refused whatever it says (it may sit in a queue or
  be held up in flight for seconds and still does nothing). The overlay starts a new epoch on every loss, Stop or
  hide, drops queued commands of an older epoch before sending them, ignores replies that belong to an older epoch,
  and claims in an epoch above the device's reported floor (`actions.epochFloor`), so even a restarted overlay
  process stays above an older one. This protects against delayed or queued traffic; it is not authentication:
  any local process can still talk to the bridge.
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

## Attended test checklist (for a full pass)

1. Pointing with clicks off; website Stop; physical stop.
2. The tile appears only in your explicit session, near the edge; passing over it does nothing; a dwell opens the menu.
3. Every item: Left, Right (one shot), Double, Drag + Drop, Drag + Cancel, Scroll + stillness exit, Pause/Stop;
   keep mode; one selection per hover; collapse after selection.
4. Cursor entry near a pending target click (rest on a target, then move onto the tile just as the dwell ends).
5. Stop, then a fresh explicit start: nothing is claimed, opened or executed until you dwell on the tile again.
6. Overlay Stop, website Stop and the physical button.
7. Applications: a text editor, Finder, a browser page; a full-screen app; a second display if you have one.
8. NodX keyboard: click a text field, open the keyboard, type letters, Shift, Backspace, Return, Close; exactly one
   character per dwell and no extra click anywhere.
9. Record the report delay (`NODX_OVERLAY_LOG` plus the recorder), missed or accidental actions and failed
   releases; stop if reports go stale.
10. Separately and attended: kill the overlay process during a drag (the device must release within 5 s) and the
    Bluetooth held-button link-loss test. Never disconnect power during a drag as part of the normal demo.
