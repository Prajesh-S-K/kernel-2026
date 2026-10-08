import test from "node:test";
import assert from "node:assert/strict";
import {
  handsFreeOf,
  interactionIdentity,
  isHandsFree,
  modeLabel,
  recoveryGuidance,
  buttonLabel,
  enableKindLabel,
  isButton,
  setupSteps,
  switchLabel,
  trainingView,
  SIMULATED_PATTERNS,
} from "../ui/handsfree.js";
import { freezeContext, invalidates, groupBlocks } from "../ui/lab.js";
import { csv, summarize } from "../ui/metrics.js";
import { DeviceClient } from "../ui/transport.js";

const profile = {
  bias: [0, 0, 0],
  deadzone: [1, 1],
  gain: [30, 30, 30, 30],
  dwellEnabled: true,
  scrollEnabled: true,
};
const geometry = { width: 800, height: 500, devicePixelRatio: 1 };
const handsFree = (over = {}) => ({
  mode: "HANDS_FREE",
  config: "VALID",
  configId: "0badc0de",
  switch: {
    present: true,
    on: true,
    permitted: true,
    switchless: false,
    switchlessStaged: false,
  },
  gesture: {
    state: "ARMED",
    last: "NONE",
    lastReject: "NONE",
    candidates: 0,
    rejected: 0,
    executed: 0,
    refused: 0,
    suppressing: false,
  },
  drag: false,
  training: {
    phase: "IDLE",
    gesture: "NONE",
    accepted: 0,
    required: 4,
    rejects: 0,
    validated: false,
    reason: "idle",
  },
  staged: [false, false],
  stored: [true, true],
  blocked: "",
  ...over,
});
const device = (over = {}, hf = handsFree()) => ({
  source: "SIMULATED",
  state: "ACTIVE",
  hasProfile: true,
  reason: "active",
  faultCode: "NONE",
  profile,
  handsFree: hf,
  ...over,
});

test("a device without hands-free telemetry is legacy and keeps its old identity", () => {
  const legacy = {
    source: "SIMULATED",
    state: "READY",
    hasProfile: true,
    profile,
  };
  assert.equal(isHandsFree(legacy), false);
  assert.deepEqual(interactionIdentity(legacy), {
    interactionMode: "LEGACY_SWITCH",
    gestureConfigId: "NONE",
  });
  assert.equal(handsFreeOf(undefined).mode, "LEGACY_SWITCH");
  assert.match(modeLabel("LEGACY_SWITCH"), /compatibility/);
  assert.equal(modeLabel("HANDS_FREE"), "HANDS-FREE");
  assert.match(switchLabel(handsFreeOf(legacy)), /legacy/);
});

test("identity freezes the configuration only in hands-free mode", () => {
  assert.deepEqual(interactionIdentity(device()), {
    interactionMode: "HANDS_FREE",
    gestureConfigId: "0badc0de",
  });
  const legacyWithRecord = device(
    {},
    handsFree({ mode: "LEGACY_SWITCH", configId: "12345678" }),
  );
  assert.equal(
    interactionIdentity(legacyWithRecord).gestureConfigId,
    "NONE",
    "a stored but unused record must not distinguish legacy blocks",
  );
});

test("setup steps follow the helper workflow and mark the next step", () => {
  const blank = {
    hasProfile: false,
    state: "CALIBRATION_REQUIRED",
    handsFree: handsFree({
      mode: "LEGACY_SWITCH",
      stored: [false, false],
      switch: { present: true, on: true, permitted: true },
    }),
  };
  let steps = setupSteps(blank);
  assert.equal(steps[0].state, "current");
  assert.deepEqual(
    steps.map((s) => s.id),
    ["calibrate", "pause", "drag", "switch", "commit", "resume"],
  );
  steps = setupSteps({ ...blank, hasProfile: true });
  assert.equal(steps[0].state, "done");
  assert.equal(steps[1].state, "current");
  const staged = {
    ...blank,
    hasProfile: true,
    handsFree: { ...blank.handsFree, staged: [true, true] },
  };
  assert.match(setupSteps(staged)[1].detail, /not saved/);
  assert.equal(setupSteps(staged)[4].state, "current");
  const saved = device({ state: "READY" });
  assert.equal(setupSteps(saved)[4].state, "done");
  assert.equal(setupSteps(saved)[5].state, "current");
  assert.equal(setupSteps(device())[5].state, "done");
});

test("an unconfigured switch is never presented as ready", () => {
  const none = device(
    {},
    handsFree({
      switch: {
        present: false,
        on: false,
        permitted: false,
        switchless: false,
      },
    }),
  );
  const step = setupSteps(none).find((s) => s.id === "switch");
  assert.equal(step.done, false);
  assert.match(step.detail, /inhibited/);
  assert.equal(switchLabel(handsFreeOf(none)), "NOT CONFIGURED");
  const qualified = device(
    {},
    handsFree({
      switch: { present: false, switchless: false, switchlessStaged: true },
    }),
  );
  assert.equal(setupSteps(qualified).find((s) => s.id === "switch").done, true);
  assert.match(
    switchLabel(
      handsFreeOf(
        device(
          {},
          handsFree({
            switch: { present: true, on: false, permitted: false },
          }),
        ),
      ),
    ),
    /OFF/,
  );
});

test("training view reports examples, retry, validation and accept rules", () => {
  const example = trainingView(
    handsFree({
      training: {
        phase: "EXAMPLE",
        gesture: "PAUSE_RESUME",
        accepted: 2,
        required: 4,
        rejects: 1,
        validated: false,
        reason: "movement too small; make it a little larger",
      },
    }),
  );
  assert.equal(example.headline, "Example 3 of 4");
  assert.match(example.detail, /too small/);
  assert.equal(example.progress, 0.5);
  assert.equal(example.canAccept, false);
  assert.equal(example.canCancel, true);
  const validate = trainingView(
    handsFree({ training: { phase: "VALIDATE", accepted: 4, required: 4 } }),
  );
  assert.match(validate.headline, /Repeat once/);
  assert.equal(validate.canAccept, false);
  const ready = trainingView(
    handsFree({
      training: { phase: "READY", accepted: 4, required: 4, validated: true },
    }),
  );
  assert.equal(ready.canAccept, true);
  const unvalidated = trainingView(
    handsFree({
      training: { phase: "READY", accepted: 4, required: 4, validated: false },
    }),
  );
  assert.equal(unvalidated.canAccept, false, "accept needs a validation");
  const failed = trainingView(
    handsFree({
      training: {
        phase: "FAILED",
        accepted: 1,
        required: 4,
        reason: "too similar to the other gesture; choose a different pattern",
      },
    }),
  );
  assert.equal(failed.failed, true);
  assert.match(failed.detail, /too similar/);
  assert.equal(failed.canCancel, true);
  const idle = trainingView(handsFree());
  assert.equal(idle.active, false);
  assert.equal(idle.progress, 0);
  assert.equal(idle.canCancel, false);
});

test("simulated patterns offered to the helper are distinct and non-empty", () => {
  assert.ok(SIMULATED_PATTERNS.length >= 3);
  assert.equal(
    new Set(SIMULATED_PATTERNS.map((p) => p.id)).size,
    SIMULATED_PATTERNS.length,
  );
});

test("recovery guidance names the cause and never promises automatic resume", () => {
  const safe = recoveryGuidance(
    device({
      state: "SAFE_STATE",
      reason: "HID connection or delivery failed",
    }),
  );
  assert.match(safe.title, /safety/);
  assert.match(safe.lines.join(" "), /20 healthy samples/);
  assert.match(safe.lines.join(" "), /Nothing resumes by itself/);
  const invalid = recoveryGuidance(
    device(
      { state: "READY" },
      handsFree({ mode: "CONFIG_INVALID", config: "CORRUPT" }),
    ),
  );
  assert.match(invalid.lines.join(" "), /CORRUPT/);
  assert.match(invalid.lines.join(" "), /legacy compatibility mode/);
  const off = recoveryGuidance(
    device(
      { state: "PAUSED" },
      handsFree({ switch: { present: true, on: false, permitted: false } }),
    ),
  );
  assert.match(off.title, /switch is OFF/);
  assert.match(off.lines.join(" "), /until you resume with your gesture/);
  const unconfigured = recoveryGuidance(
    device(
      { state: "READY" },
      handsFree({
        switch: { present: false, permitted: false, switchless: false },
      }),
    ),
  );
  assert.match(unconfigured.title, /No enable switch/);
  const paused = recoveryGuidance(
    device(
      { state: "PAUSED" },
      handsFree({ blocked: "waiting for healthy sensor samples" }),
    ),
  );
  assert.match(paused.lines.join(" "), /pause \/ resume gesture/);
  assert.match(
    paused.lines.join(" "),
    /Resume is blocked: waiting for healthy/,
  );
  const active = recoveryGuidance(device());
  assert.equal(active.title, "Active");
  assert.match(active.lines.join(" "), /dwell clicks automatically/i);
  const dragging = recoveryGuidance(device({}, handsFree({ drag: true })));
  assert.match(dragging.lines.join(" "), /release/);
  assert.match(
    recoveryGuidance(device({ state: "TRAINING" })).title,
    /Training/,
  );
  assert.match(
    recoveryGuidance({ hasProfile: false, state: "CALIBRATION_REQUIRED" })
      .title,
    /Calibration/,
  );
  const legacy = recoveryGuidance(
    device({ state: "READY" }, handsFree({ mode: "LEGACY_SWITCH" })),
  );
  assert.match(legacy.lines.join(" "), /legacy compatibility/);
});

test("a failed save stays visible as an error with the earlier setup kept", () => {
  const failed = recoveryGuidance(
    device({ faultCode: "STORAGE", state: "READY" }),
  );
  assert.equal(failed.title, "Saving failed");
  assert.match(failed.lines.join(" "), /earlier configuration was kept/);
});

test("hands-free blocks freeze mode and configuration identity with dwell selection", () => {
  const context = freezeContext(device(), "SIMULATED", "ADAPTIVE", geometry);
  assert.equal(context.interactionMode, "HANDS_FREE");
  assert.equal(context.gestureConfigId, "0badc0de");
  assert.equal(context.selectionMethod, "DWELL");
  assert.throws(() => {
    context.interactionMode = "LEGACY_SWITCH";
  });
  const host = freezeContext(device(), "HOST_POINTER", "ADAPTIVE", geometry);
  assert.equal(host.selectionMethod, "HOST_CLICK");
  const legacy = freezeContext(
    device({}, handsFree({ mode: "LEGACY_SWITCH" })),
    "SIMULATED",
    "ADAPTIVE",
    geometry,
  );
  assert.equal(legacy.selectionMethod, "SWITCH_OR_DWELL");
  assert.equal(legacy.gestureConfigId, "NONE");
});

test("configuration, mode and gesture changes invalidate an active block", () => {
  const live = device();
  const context = freezeContext(live, "SIMULATED", "ADAPTIVE", geometry);
  const baseline = { executed: 0 };
  assert.equal(invalidates(context, live, geometry, baseline), "");
  assert.equal(
    invalidates(
      context,
      device({}, handsFree({ configId: "feedface" })),
      geometry,
      baseline,
    ),
    "gesture configuration changed",
  );
  assert.equal(
    invalidates(
      context,
      device({}, handsFree({ mode: "LEGACY_SWITCH" })),
      geometry,
      baseline,
    ),
    "interaction mode changed",
  );
  assert.equal(
    invalidates(
      context,
      device({}, handsFree({ drag: true })),
      geometry,
      baseline,
    ),
    "drag active",
  );
  const executed = handsFree();
  executed.gesture.executed = 1;
  assert.equal(
    invalidates(context, device({}, executed), geometry, baseline),
    "gesture command executed",
  );
  assert.equal(
    invalidates(context, device({ state: "PAUSED" }), geometry, baseline),
    "device stopped",
    "a pause gesture stops the block",
  );
  assert.equal(
    invalidates(context, device({ state: "SAFE_STATE" }), geometry, baseline),
    "device fault",
  );
  assert.equal(
    invalidates(context, live, geometry),
    "",
    "baseline is optional",
  );
});

test("legacy and hands-free results are never pooled into one block", () => {
  const base = {
    blockId: "same",
    deviceSource: "SIMULATED",
    inputSource: "SIMULATED",
    condition: "ADAPTIVE",
    selectionMethod: "DWELL",
    profile,
  };
  const groups = groupBlocks([
    { ...base, interactionMode: "HANDS_FREE", gestureConfigId: "aaaaaaaa" },
    { ...base, interactionMode: "HANDS_FREE", gestureConfigId: "bbbbbbbb" },
    { ...base, interactionMode: "LEGACY_SWITCH", gestureConfigId: "NONE" },
    { ...base },
    { ...base, interactionMode: "HANDS_FREE", gestureConfigId: "aaaaaaaa" },
  ]);
  assert.equal(groups.length, 4);
  assert.equal(
    groups.find((g) => g[0].gestureConfigId === "aaaaaaaa").length,
    2,
  );
});

test("raw export keeps interaction identity, gesture interruptions and abort reasons", () => {
  const rows = [
    {
      trial: 1,
      hit: true,
      aborted: false,
      distance: 100,
      width: 50,
      movementTimeMs: 1500,
      interactionMode: "HANDS_FREE",
      gestureConfigId: "0badc0de",
      gestureInterruptions: 2,
      abortReason: "",
    },
    {
      trial: 2,
      hit: false,
      aborted: true,
      distance: 100,
      width: 50,
      movementTimeMs: 400,
      interactionMode: "HANDS_FREE",
      gestureConfigId: "0badc0de",
      gestureInterruptions: 1,
      abortReason: "gesture command executed",
    },
  ];
  const text = csv(rows);
  for (const column of [
    "interactionMode",
    "gestureConfigId",
    "gestureInterruptions",
    "abortReason",
  ])
    assert.ok(text.split("\n")[0].includes(`"${column}"`), column);
  assert.match(text, /gesture command executed/);
  const summary = summarize(rows);
  assert.equal(
    summary.attempts,
    1,
    "aborted attempts are retained but not summarised",
  );
});

test("setup commands are queued, never coalesced like polls", async () => {
  const sent = [];
  const client = new DeviceClient(async (_endpoint, options) => {
    sent.push(JSON.parse(options.body).action);
    return { ok: true, json: async () => ({ ok: true }) };
  });
  await Promise.all([
    client.request({ action: "train", op: "start", gesture: "pause" }),
    client.request({ action: "train", op: "cancel" }),
    client.request({ action: "handsfree", op: "commit" }),
    client.request({ action: "enable", enabled: false }),
  ]);
  assert.deepEqual(sent, ["train", "train", "handsfree", "enable"]);
  let release;
  const slow = new DeviceClient(async () => {
    await new Promise((resolve) => {
      release = resolve;
    });
    return { ok: true, json: async () => ({ ok: true }) };
  });
  const poll = slow.request({ action: "step", enabled: true });
  assert.equal(
    slow.request({ action: "status" }),
    poll,
    "polls still coalesce",
  );
  await Promise.resolve();
  release();
  await poll;
});

test("the Lab logs interaction identity, gesture interruptions and the abort reason", async () => {
  const { createPerformanceLab } = await import("../ui/lab-view.js");
  const nodes = new Map();
  const node = () => ({
    value: "",
    hidden: false,
    disabled: false,
    textContent: "",
    style: {},
    classList: { toggle() {} },
    getBoundingClientRect: () => ({ width: 800, height: 500 }),
    replaceChildren() {},
    append() {},
  });
  const $ = (id) => {
    if (!nodes.has(id)) nodes.set(id, node());
    return nodes.get(id);
  };
  $("condition").value = "ADAPTIVE";
  $("inputSource").value = "SIMULATED";
  const previousDocument = globalThis.document;
  globalThis.document = { createElement: node };
  let current = device();
  const logged = [];
  const lab = createPerformanceLab({
    $,
    geometry: () => geometry,
    getDevice: () => current,
    getPoint: () => ({ x: 400, y: 250 }),
    setPoint: () => {},
    renderCursor: () => {},
    toast: () => {},
    download: () => {},
    action: async () => ({ ...current, ok: true }),
    request: async (row) => {
      logged.push(row);
      return { profileHash: "h" };
    },
  });
  try {
    await lab.start();
    assert.equal(lab.running, true);
    lab.select(400, 250); // the centre start target
    const interrupted = handsFree();
    interrupted.gesture.candidates = 3; // recognition opened during the attempt
    current = device({}, interrupted);
    lab.select(400, 250);
    assert.equal(logged[0].interactionMode, "HANDS_FREE");
    assert.equal(logged[0].gestureConfigId, "0badc0de");
    assert.equal(logged[0].selectionMethod, "DWELL");
    assert.equal(logged[0].gestureInterruptions, 3);
    assert.equal(logged[0].blockContext.interactionMode, "HANDS_FREE");
    // A configuration change aborts the block and keeps the reason in the raw row.
    lab.check(device({}, handsFree({ configId: "feedface" })));
    assert.equal(lab.running, false);
    const aborted = logged.at(-1);
    assert.equal(aborted.aborted, true);
    assert.equal(aborted.abortReason, "gesture configuration changed");
    assert.equal(
      aborted.gestureConfigId,
      "0badc0de",
      "rows keep the frozen identity",
    );
  } finally {
    globalThis.document = previousDocument;
  }
});

const button = (over = {}) =>
  handsFree({
    switch: {
      present: true,
      on: false,
      permitted: false,
      switchless: false,
      switchlessStaged: false,
      kind: "MOMENTARY",
      kindStaged: "MOMENTARY",
      pressed: false,
      latched: false,
      armed: true,
      ...over,
    },
  });

test("older engines and maintained records read as a maintained switch, never a button", () => {
  assert.equal(isButton(handsFreeOf({})), false);
  assert.equal(isButton(handsFreeOf({ handsFree: handsFree() })), false);
  assert.match(
    switchLabel(handsFreeOf({ handsFree: handsFree() })),
    /ON · control permitted/,
  );
  assert.equal(buttonLabel(handsFreeOf({ handsFree: handsFree() })), "");
});

test("the raw button state and the latched permission are labelled separately", () => {
  const held = handsFreeOf({
    handsFree: button({
      pressed: true,
      latched: true,
      permitted: true,
      on: true,
    }),
  });
  assert.equal(buttonLabel(held), "PRESSED");
  assert.match(switchLabel(held), /PERMITTED/);
  const released = handsFreeOf({
    handsFree: button({
      pressed: false,
      latched: true,
      permitted: true,
      on: true,
    }),
  });
  assert.equal(buttonLabel(released), "released");
  assert.match(
    switchLabel(released),
    /PERMITTED/,
    "released does not mean disabled",
  );
  const disabled = handsFreeOf({ handsFree: button() });
  assert.match(switchLabel(disabled), /DISABLED · press enable button/);
  assert.equal(buttonLabel(disabled), "released");
});

test("button guidance: disabled explains press, never-resume, and loss on restart or fault", () => {
  const guide = recoveryGuidance({
    state: "PAUSED",
    hasProfile: true,
    faultCode: "NONE",
    handsFree: button(),
  });
  assert.match(guide.title, /disabled/i);
  const text = guide.lines.join(" ");
  assert.match(text, /Press the enable button once/);
  assert.match(text, /does not start control/);
  assert.match(text, /never saved/);
  assert.match(text, /held when the device started/);
});

test("active guidance tells the user the button disables control at once", () => {
  const guide = recoveryGuidance({
    state: "ACTIVE",
    hasProfile: true,
    faultCode: "NONE",
    handsFree: button({ permitted: true, latched: true, on: true }),
  });
  assert.match(
    guide.lines.join(" "),
    /press the enable button to disable control at once/,
  );
});

test("the setup step names the input kind and flags an unsaved change of kind", () => {
  const saved = setupSteps({
    hasProfile: true,
    state: "READY",
    handsFree: button(),
  });
  assert.match(saved.find((s) => s.id === "switch").detail, /Push button/);
  const staged = setupSteps({
    hasProfile: true,
    state: "READY",
    handsFree: button({ kind: "MAINTAINED", kindStaged: "MOMENTARY" }),
  });
  assert.match(
    staged.find((s) => s.id === "switch").detail,
    /staged.*Not saved yet/,
  );
  assert.match(enableKindLabel("MAINTAINED"), /compatibility/);
});

test("a Lab block freezes the gesture configuration whose identity includes the input kind", () => {
  const a = { source: "SIMULATED", profile, handsFree: button() };
  const frozen = freezeContext(a, "SIMULATED", "ADAPTIVE", geometry);
  const sameConfig = { ...a };
  assert.equal(
    invalidates(frozen, sameConfig, geometry, { executed: 0 }) === "",
    false,
    "inactive device stops",
  );
  const other = { ...a, handsFree: { ...button(), configId: "feedface" } };
  assert.equal(
    invalidates(
      frozen,
      { ...other, state: "ACTIVE", hasProfile: true },
      geometry,
      { executed: 0 },
    ),
    "gesture configuration changed",
  );
});

test("the movement-only demo is shown in guidance and defaults to off for older engines", () => {
  assert.equal(handsFreeOf({}).demoMovementOnly, false);
  const on = recoveryGuidance({
    state: "ACTIVE",
    hasProfile: true,
    faultCode: "NONE",
    handsFree: handsFree({ demoMovementOnly: true }),
  });
  assert.match(on.lines.join(" "), /MOVEMENT-ONLY DEMO/);
  assert.match(
    on.lines.join(" "),
    /clicks, drag and scrolling are switched off/,
  );
  const off = recoveryGuidance({
    state: "ACTIVE",
    hasProfile: true,
    faultCode: "NONE",
    handsFree: handsFree(),
  });
  assert.doesNotMatch(off.lines.join(" "), /MOVEMENT-ONLY/);
});

import { BANNER, mappingLines, pauseOnBlur, labBlocked, uncalView, ROTATION_GUIDE } from "../ui/uncal.js";
const hw = (uncal, extra = {}) => ({
  source: "HARDWARE",
  connected: true,
  handsFree: { uncalDemo: { active: false, present: true, permitted: true, blocked: "", profileState: "MISSING", gain: 12, deadzone: 2.5, maxStep: 4, ...uncal } },
  axes: { valid: true, gyro: [2, 0, 1], gyroSigns: [1, 1, 1], accel: [1, 0, 2], accelSigns: [1, 1, -1] },
  ...extra,
});
test("uncalibrated demo: only offered for live hardware and never started by the page", () => {
  assert.equal(uncalView({ source: "SIMULATED", handsFree: {} }).hardware, false);
  assert.equal(uncalView(undefined).canStart, false);
  assert.equal(uncalView(hw({})).canStart, true);
  assert.equal(uncalView(hw({ blocked: "press the enable button first" })).canStart, false);
  assert.equal(uncalView(hw({}, { connected: false })).canStart, false);
  assert.match(uncalView(hw({ blocked: "BLE link unavailable" })).status, /BLE link unavailable/);
});
test("uncalibrated demo: banner text appears exactly while active", () => {
  assert.equal(uncalView(hw({})).banner, "");
  assert.equal(uncalView(hw({ active: true })).banner, BANNER);
  assert.equal(BANNER, "UNCALIBRATED DEMO — LIVE SENSOR");
  assert.equal(uncalView(hw({ active: true })).canStart, false);
});
test("uncalibrated demo: a corrupt profile is reported as corrupt, not repaired", () => {
  assert.match(uncalView(hw({ profileState: "CORRUPT" })).calibration, /CORRUPT/);
  assert.match(uncalView(hw({ profileState: "MISSING" })).calibration, /not a calibration/);
});
test("uncalibrated demo: shows the reported axis mapping and the rotation guide", () => {
  const lines = mappingLines(hw({}));
  assert.match(lines[0], /Yaw.*Z axis, sign \+/);
  assert.match(lines[1], /Pitch.*X axis/);
  assert.match(lines[2], /Roll.*Y axis/);
  assert.match(lines.at(-1), /Mapping valid/);
  assert.match(mappingLines(hw({}, { axes: { valid: false, gyro: [0, 0, 0], gyroSigns: [1, 1, 1] } })).at(-1), /INVALID/);
  assert.match(mappingLines({})[0], /did not report/);
  assert.equal(ROTATION_GUIDE.length, 5);
});
test("uncalibrated demo: keeps focus-loss pause for everything except the active demo; out of the lab", () => {
  assert.equal(pauseOnBlur(hw({})), true);
  assert.equal(pauseOnBlur({ source: "SIMULATED" }), true);
  assert.equal(pauseOnBlur(hw({ active: true })), false);
  assert.equal(labBlocked(hw({ active: true })), true);
  assert.equal(labBlocked(hw({})), false);
});
