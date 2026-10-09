"""NodX desktop action overlay (EXPERIMENTAL, macOS only, optional).

A small floating tile that stays above other applications and lets the head-controlled pointer choose
Left-click, Right-click, Double-click, Drag, Drop, Scroll, Keyboard, Cancel and Pause/Stop. It talks only to
the existing local companion bridge over HTTP (it never opens a serial port and never injects mouse events);
the firmware stays the single authority for the dwell target actions, the held-button state and the
SafetyManager -> HIDManager -> BLE output.

`logic`, `bridge` and `controller` have no native dependency and are unit tested everywhere; `app` needs the
optional PyObjC packages in overlay/requirements.txt.
"""
