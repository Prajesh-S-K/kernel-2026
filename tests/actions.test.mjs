import assert from "node:assert/strict";
import test from "node:test";
import {
  ACTIONS_LABEL,
  CONTROLS,
  HoverReporter,
  INHIBIT_TEXT,
  KEEP_NOTE,
  LINK_NOTE,
  LatencyStats,
  PALETTE_NOTE,
  SCROLL_EXIT_TEXT,
  SCROLL_NOTE,
  actionsOf,
  bannerOf,
  controlsOf,
  panelView,
  selectedText,
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
  keep: false,
  locked: "NONE",
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
    drop: 0,
    cancel: 0,
  },
  link: {
    reporting: true,
    ageMs: 0,
    pending: false,
    commitMs: 150,
    inhibited: 0,
    cancelled: 0,
    paletteReleases: 0,
    confirmedReleases: 0,
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

test("the controls are Left, Right, Double, Drag, Drop, Cancel, Scroll and Stop", () => {
  assert.deepEqual(
    CONTROLS.map((c) => c.id),
    ["left", "right", "double", "drag", "drop", "cancel", "scroll", "stop"],
  );
  assert.deepEqual(
    CONTROLS.map((c) => c.label),
    [
      "Left-click",
      "Right-click",
      "Double-click",
      "Drag",
      "Drop",
      "Cancel",
      "Scroll",
      "Stop",
    ],
  );
});

test("Drop and Cancel are never 'selected' modes and are highlighted only while a drag is held", () => {
  const idle = controlsOf(frame({ ...base, mode: "LEFT" }));
  for (const id of ["drop", "cancel"]) {
    const control = idle.find((c) => c.id === id);
    assert.equal(control.selected, false);
    assert.equal(control.urgent, false);
  }
  const held = controlsOf(frame({ ...base, mode: "DRAG", dragging: true }));
  for (const id of ["drop", "cancel"])
    assert.equal(held.find((c) => c.id === id).urgent, true);
  assert.equal(held.find((c) => c.id === "left").urgent, false);
  assert.match(
    CONTROLS.find((c) => c.id === "drop").hint,
    /No movement needed/,
  );
  assert.match(
    CONTROLS.find((c) => c.id === "cancel").hint,
    /clear anything waiting/,
  );
});

test("one selection per hover: a just-chosen control shows as locked with no ring", () => {
  const controls = controlsOf(
    frame({
      ...base,
      mode: "RIGHT",
      hover: "RIGHT",
      inPalette: true,
      locked: "RIGHT",
      dwell: { state: "LOCKOUT", progress: 0.7 },
    }),
  );
  const right = controls.find((c) => c.id === "right");
  assert.equal(right.locked, true);
  assert.equal(right.progress, 0);
  assert.equal(controls.filter((c) => c.locked).length, 1);
  const free = controlsOf(
    frame({
      ...base,
      hover: "RIGHT",
      locked: "NONE",
      dwell: { state: "PROGRESS", progress: 0.4 },
    }),
  );
  assert.equal(free.find((c) => c.id === "right").locked, false);
  assert.equal(free.find((c) => c.id === "right").progress, 0.4);
});

test("the selected action is stated in words and DRAGGING is unmistakable", () => {
  assert.equal(
    selectedText(frame({ ...base, mode: "LEFT" })),
    "Selected action: LEFT-CLICK",
  );
  assert.match(selectedText(frame({ ...base, mode: "RIGHT" })), /RIGHT-CLICK/);
  assert.match(
    selectedText(frame({ ...base, mode: "DRAG", dragging: true })),
    /DRAG, button HELD/,
  );
  assert.match(
    selectedText(frame({ ...base, mode: "SCROLL", scroll: "ACTIVE" })),
    /SCROLL, running/,
  );
  assert.match(selectedText(frame({ ...base, enabled: false })), /palette off/);
  assert.equal(
    bannerOf(frame({ ...base, mode: "DRAG", dragging: true })).text,
    "DRAGGING",
  );
});

test("target actions are visibly inhibited while the pointer is on the palette", () => {
  const on = bannerOf(frame({ ...base, inPalette: true, hover: "LEFT" }));
  assert.equal(on.detail, INHIBIT_TEXT);
  assert.match(INHIBIT_TEXT, /inhibited while the pointer is on the palette/);
  assert.equal(bannerOf(frame({ ...base, inPalette: false })).detail, "");
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
        drop: 2,
        cancel: 1,
      },
    }),
  );
  for (const part of [
    "Dwell 1200 ms",
    "left 3",
    "right 2",
    "double 1",
    "drag 4/3",
    "drop 2",
    "cancel 1",
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

test("keep selected action: off by default, shown as one shot or kept, never for Drag or Scroll", () => {
  assert.equal(actionsOf(frame(base)).keep, false);
  assert.equal(actionsOf({}).keep, false);
  assert.equal(
    selectedText(frame({ ...base, mode: "RIGHT" })),
    "Selected action: RIGHT-CLICK · one shot",
  );
  assert.equal(
    selectedText(frame({ ...base, mode: "RIGHT", keep: true })),
    "Selected action: RIGHT-CLICK · KEPT",
  );
  assert.match(
    selectedText(frame({ ...base, mode: "DOUBLE", keep: true })),
    /DOUBLE-CLICK · KEPT/,
  );
  assert.match(
    bannerOf(frame({ ...base, mode: "DOUBLE", keep: true })).text,
    /\(kept\)/,
  );
  assert.match(
    bannerOf(frame({ ...base, mode: "RIGHT" })).text,
    /\(one shot\)/,
  );
  // Left, Drag and Scroll are never labelled kept or one shot
  for (const mode of ["LEFT", "DRAG", "SCROLL"]) {
    const text = selectedText(frame({ ...base, mode, keep: true }));
    assert.doesNotMatch(text, /KEPT|one shot/, mode);
  }
  assert.match(KEEP_NOTE, /off by default/);
  assert.match(KEEP_NOTE, /never applies to Drag or Scroll/);
  assert.match(KEEP_NOTE, /off again at every session start/);
});

test("the palette is described as an ordinary browser window with no always-on-top guarantee", () => {
  assert.match(PALETTE_NOTE, /ordinary browser window/);
  assert.match(PALETTE_NOTE, /cannot guarantee always-on-top/);
  assert.match(PALETTE_NOTE, /not a system-wide overlay/);
});

test("hover reporter: reports on enter and leave, repeats while hovering AND while outside, ignores stale leaves", () => {
  const sent = [];
  const r = new HoverReporter((target) => sent.push(target), 500, 1000);
  r.enter("left", 0);
  r.enter("left", 10); // already there: nothing new
  assert.deepEqual(sent, ["left"]);
  r.tick(300);
  assert.deepEqual(sent, ["left"]); // not yet time to refresh
  r.tick(520);
  assert.deepEqual(sent, ["left", "left"]); // refreshed so the device keeps a fresh report
  r.enter("right", 600); // moved straight to the neighbour
  r.leave("left", 601); // the old control's leave arrives late: must not clear the new hover
  assert.deepEqual(sent, ["left", "left", "right"]);
  r.leave("right", 700);
  assert.deepEqual(sent, ["left", "left", "right", "none"]);
  r.tick(1000); // outside: the heartbeat continues, more slowly, so "outside" is a FRESH report
  assert.deepEqual(sent, ["left", "left", "right", "none"]);
  r.tick(1750);
  assert.deepEqual(sent, ["left", "left", "right", "none", "none"]);
  r.leave("right", 1800); // not on it any more: nothing new
  assert.equal(sent.length, 5);
});

test("hover reporter: a just-enabled palette reports at once (kick)", () => {
  const sent = [];
  const r = new HoverReporter((target) => sent.push(target));
  r.tick(0);
  r.tick(10);
  assert.deepEqual(sent, ["none"]);
  r.kick();
  r.tick(20);
  assert.deepEqual(sent, ["none", "none"]);
});

test("latency statistics: median, 95th percentile, worst, lost reports and the risk warning", () => {
  const stats = new LatencyStats();
  assert.match(stats.line(), /no reports yet/);
  for (let i = 1; i <= 100; i += 1) stats.add(i * 10); // 10 .. 1000 ms
  const s = stats.summary();
  assert.equal(s.count, 100);
  assert.equal(s.worst, 1000);
  assert.ok(s.median >= 500 && s.median <= 520);
  assert.ok(s.p95 >= 950);
  assert.doesNotMatch(stats.line(), /could reach the target/);
  stats.add(1800);
  assert.match(stats.line(), /could reach the target/);
  const lost = new LatencyStats();
  lost.add(30);
  lost.fail();
  assert.equal(lost.summary().failed, 1);
  assert.match(lost.line(), /lost 1 of 2/);
  assert.match(lost.line(), /could reach the target/);
  const ring = new LatencyStats(3);
  for (const ms of [1, 2, 3, 4, 5]) ring.add(ms);
  assert.equal(ring.summary().worst, 5);
  assert.equal(ring.samples.length, 3);
});

test("scroll shows HOLD STILL TO EXIT SCROLLING prominently and says that reading pauses exit", () => {
  const banner = bannerOf(
    frame({ ...base, mode: "SCROLL", scroll: "ACTIVE", frozen: true }),
  );
  assert.equal(banner.kind, "scrolling");
  assert.equal(banner.exitHint, "HOLD STILL TO EXIT SCROLLING");
  assert.equal(SCROLL_EXIT_TEXT, "HOLD STILL TO EXIT SCROLLING");
  assert.match(banner.detail, /pausing to read also exits/i);
  assert.match(SCROLL_NOTE, /Pausing to read ALSO exits Scroll/);
  assert.equal(bannerOf(frame(base)).exitHint, undefined);
});

test("the link warning: no fresh report is shown loudly and the limits are stated", () => {
  const quiet = frame({ ...base, link: { reporting: false, ageMs: 0 } });
  assert.equal(bannerOf(quiet).kind, "noreport");
  assert.match(bannerOf(quiet).text, /NOT REPORTING/);
  assert.match(panelView(quiet).link, /NO fresh palette report/);
  const live = frame({
    ...base,
    link: {
      reporting: true,
      ageMs: 120,
      inhibited: 2,
      cancelled: 1,
      paletteReleases: 3,
    },
  });
  assert.equal(bannerOf(live).kind, "ready");
  assert.match(panelView(live).link, /report age 120 ms/);
  assert.match(panelView(live).link, /drags released on palette entry 3/);
  assert.match(LINK_NOTE, /NOT a guarantee against click-through/);
  assert.match(LINK_NOTE, /1\.6 s/);
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
  assert.match(source, /'frame'/);
  // a blur must never report "outside" while the pointer can still be over the window
  assert.doesNotMatch(source, /addEventListener\('blur'/);
});
