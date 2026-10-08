import assert from "node:assert/strict";
import test from "node:test";
import {
  ACTIONS_LABEL,
  CONTROLS,
  HoverReporter,
  PALETTE_NOTE,
  SCROLL_NOTE,
  actionsOf,
  bannerOf,
  controlsOf,
  panelView,
  statsLine,
} from "../ui/actions.js";

const frame = (actions, source = "HARDWARE") => ({ source, actions });
const base = {
  enabled: true,
  mode: "LEFT",
  dragging: false,
  scroll: "OFF",
  frozen: false,
  hover: "NONE",
  inPalette: false,
  last: "NONE",
  selections: 0,
  dwellMs: 1200,
  tolerance: 8,
  neutral: 3,
  dwell: { state: "IDLE", progress: 0 },
  exit: { progress: 0 },
  counts: {
    left: 0,
    right: 0,
    double: 0,
    dragStart: 0,
    dragRelease: 0,
    scrollStart: 0,
    scrollExit: 0,
    wheel: 0,
  },
  blocked: "",
};

test("missing or malformed palette data never throws and reads as off", () => {
  for (const device of [
    undefined,
    null,
    {},
    { actions: 5 },
    { actions: { enabled: "yes", dwell: 3 } },
  ]) {
    const a = actionsOf(device);
    assert.equal(a.enabled, false);
    assert.equal(a.mode, "LEFT");
    assert.equal(a.dwell.progress, 0);
    assert.equal(bannerOf(device).kind, "off");
    assert.equal(controlsOf(device).length, CONTROLS.length);
  }
  assert.equal(
    actionsOf({ actions: { dwell: { progress: 7 }, exit: { progress: -2 } } })
      .dwell.progress,
    1,
  );
  assert.equal(
    actionsOf({ actions: { dwell: { progress: 7 }, exit: { progress: -2 } } })
      .exitProgress,
    0,
  );
});

test("the six controls are exactly Left, Right, Double, Drag, Scroll and Stop", () => {
  assert.deepEqual(
    CONTROLS.map((c) => c.id),
    ["left", "right", "double", "drag", "scroll", "stop"],
  );
  assert.deepEqual(
    CONTROLS.map((c) => c.label),
    ["Left-click", "Right-click", "Double-click", "Drag", "Scroll", "Stop"],
  );
});

test("Left-click is the visible default and the selected control follows the mode", () => {
  let selected = controlsOf(frame(base))
    .filter((c) => c.selected)
    .map((c) => c.id);
  assert.deepEqual(selected, ["left"]);
  for (const [mode, id] of [
    ["RIGHT", "right"],
    ["DOUBLE", "double"],
    ["DRAG", "drag"],
    ["SCROLL", "scroll"],
  ]) {
    selected = controlsOf(frame({ ...base, mode }))
      .filter((c) => c.selected)
      .map((c) => c.id);
    assert.deepEqual(selected, [id], mode);
  }
  // Stop is never "selected": it ends the session
  assert.equal(
    controlsOf(frame({ ...base, mode: "STOP" })).some((c) => c.selected),
    false,
  );
  // everything is off while disabled
  assert.equal(
    controlsOf(frame({ ...base, enabled: false })).some((c) => c.selected),
    false,
  );
});

test("DRAGGING and SCROLLING are prominent banners; scroll armed and ready are distinct", () => {
  assert.deepEqual(
    [
      bannerOf(frame({ ...base, dragging: true, mode: "DRAG" })).kind,
      bannerOf(frame({ ...base, dragging: true, mode: "DRAG" })).text,
    ],
    ["dragging", "DRAGGING"],
  );
  assert.equal(
    bannerOf(frame({ ...base, scroll: "ACTIVE", frozen: true, mode: "SCROLL" }))
      .text,
    "SCROLLING",
  );
  assert.equal(
    bannerOf(frame({ ...base, scroll: "ARMED", mode: "SCROLL" })).kind,
    "armed",
  );
  assert.equal(bannerOf(frame(base)).text, "LEFT-CLICK");
  assert.match(bannerOf(frame({ ...base, mode: "RIGHT" })).text, /one shot/);
  assert.match(bannerOf(frame({ ...base, mode: "DOUBLE" })).text, /one shot/);
  // dragging wins over everything else a stale frame might also say
  assert.equal(
    bannerOf(frame({ ...base, dragging: true, scroll: "ARMED" })).kind,
    "dragging",
  );
});

test("hover shows the dwell progress on that control only", () => {
  const controls = controlsOf(
    frame({
      ...base,
      hover: "RIGHT",
      inPalette: true,
      dwell: { state: "PROGRESS", progress: 0.5 },
    }),
  );
  const hovered = controls.filter((c) => c.hovered);
  assert.deepEqual(
    hovered.map((c) => c.id),
    ["right"],
  );
  assert.equal(hovered[0].progress, 0.5);
  assert.equal(controls.filter((c) => c.progress > 0).length, 1);
});

test("while scrolling the Scroll control becomes the Exit control with the exit progress", () => {
  const controls = controlsOf(
    frame({
      ...base,
      mode: "SCROLL",
      scroll: "ACTIVE",
      frozen: true,
      exit: { progress: 0.75 },
      dwell: { state: "IDLE", progress: 0 },
    }),
  );
  const scroll = controls.find((c) => c.id === "scroll");
  assert.equal(scroll.label, "Exit Scroll");
  assert.equal(scroll.exit, true);
  assert.equal(scroll.progress, 0.75);
  assert.match(scroll.hint, /hold still/i);
  // Stop is still present and unchanged
  const stop = controls.find((c) => c.id === "stop");
  assert.equal(stop.label, "Stop");
  // armed scroll is not an exit control
  assert.equal(
    controlsOf(frame({ ...base, mode: "SCROLL", scroll: "ARMED" })).find(
      (c) => c.id === "scroll",
    ).exit,
    false,
  );
});

test("the drag control is marked active only while the button is held", () => {
  assert.equal(
    controlsOf(frame({ ...base, mode: "DRAG" })).find((c) => c.id === "drag")
      .active,
    false,
  );
  assert.equal(
    controlsOf(frame({ ...base, mode: "DRAG", dragging: true })).find(
      (c) => c.id === "drag",
    ).active,
    true,
  );
});

test("panel: enabling needs hardware, a report and no blocker; the blocker is shown", () => {
  assert.equal(
    panelView(frame({ ...base, enabled: false, blocked: "" })).canEnable,
    true,
  );
  assert.equal(
    panelView(
      frame({
        ...base,
        enabled: false,
        blocked: "start the control session first",
      }),
    ).canEnable,
    false,
  );
  assert.equal(
    panelView(
      frame({
        ...base,
        enabled: false,
        blocked: "start the control session first",
      }),
    ).enableBlocked,
    "start the control session first",
  );
  assert.equal(
    panelView(frame({ ...base, enabled: false }, "SIMULATED")).canEnable,
    false,
  );
  assert.equal(panelView({ source: "HARDWARE" }).canEnable, false);
  assert.equal(panelView(frame(base)).enableBlocked, "");
});

test("stats report every counter and the one dwell duration", () => {
  const line = statsLine(
    frame({
      ...base,
      counts: {
        ...base.counts,
        left: 3,
        right: 2,
        double: 1,
        dragStart: 4,
        dragRelease: 3,
        scrollStart: 2,
        scrollExit: 1,
        wheel: 17,
      },
    }),
  );
  for (const part of [
    "Dwell 1200 ms",
    "left 3",
    "right 2",
    "double 1",
    "drag 4/3",
    "scroll 2 started",
    "1 exited",
    "17 wheel",
  ]) {
    assert.ok(line.includes(part), part);
  }
});

test("wording: experimental, no system-wide overlay claim, scroll exit explained", () => {
  assert.match(ACTIONS_LABEL, /EXPERIMENTAL/);
  assert.match(PALETTE_NOTE, /not a system-wide overlay/);
  assert.match(PALETTE_NOTE, /beside/);
  assert.match(SCROLL_NOTE, /hold still/);
  assert.match(SCROLL_NOTE, /frozen pointer cannot travel/);
  for (const text of [PALETTE_NOTE, SCROLL_NOTE]) {
    assert.doesNotMatch(
      text,
      /always on top|system-wide overlay\.\s*It is on/i,
    );
  }
});

test("hover reporter: reports on enter and leave, refreshes while hovering, ignores stale leaves", () => {
  const sent = [];
  const r = new HoverReporter((target) => sent.push(target), 500);
  r.enter("left", 0);
  r.enter("left", 10); // already there: nothing new
  assert.deepEqual(sent, ["left"]);
  r.tick(300);
  assert.deepEqual(sent, ["left"]); // not yet time to refresh
  r.tick(520);
  assert.deepEqual(sent, ["left", "left"]); // refreshed so the device does not expire it
  r.enter("right", 600); // moved straight to the neighbour
  r.leave("left", 601); // the old control's leave arrives late: must not clear the new hover
  assert.deepEqual(sent, ["left", "left", "right"]);
  r.leave("right", 700);
  assert.deepEqual(sent, ["left", "left", "right", "none"]);
  r.tick(5000);
  r.leave("right", 5001);
  assert.deepEqual(sent, ["left", "left", "right", "none"]); // nothing while not hovering
});

test("hover reporter reset (window left, hidden or closed) always reports leaving", () => {
  const sent = [];
  const r = new HoverReporter((target) => sent.push(target));
  r.reset(0); // nothing to clear
  assert.deepEqual(sent, []);
  r.enter("stop", 1);
  r.reset(2);
  assert.deepEqual(sent, ["stop", "none"]);
  r.reset(3);
  assert.deepEqual(sent, ["stop", "none"]);
});

test("the palette window has no click handler on its controls", async () => {
  const fs = await import("node:fs");
  const source = fs.readFileSync(
    new URL("../ui/palette.js", import.meta.url),
    "utf8",
  );
  assert.doesNotMatch(
    source,
    /addEventListener\('(click|mousedown|mouseup|pointerdown|pointerup|dblclick|contextmenu)'/,
  );
  assert.doesNotMatch(source, /\.onclick|\.onmousedown/);
  assert.match(source, /mouseenter/);
  assert.match(source, /mouseleave/);
});
