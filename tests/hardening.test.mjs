import test from "node:test";
import assert from "node:assert/strict";
import {
  freezeContext,
  invalidates,
  groupBlocks,
  comparisonSettings,
} from "../ui/lab.js";
import { sourceLabels } from "../ui/source.js";
import { DeviceClient } from "../ui/transport.js";
const device = {
  source: "SIMULATED",
  state: "ACTIVE",
  profile: {
    bias: [0, 0, 0],
    deadzone: [1, 1],
    gain: [30, 30, 30, 30],
    dwellEnabled: true,
    scrollEnabled: false,
  },
};
const geometry = { width: 800, height: 500, devicePixelRatio: 1 };
test("frozen block captures profile source selection and geometry", () => {
  const context = freezeContext(device, "SIMULATED", "ADAPTIVE", geometry);
  assert.throws(() => {
    context.profile.gain[0] = 99;
  });
  assert.equal(context.selectionMethod, "SWITCH_OR_DWELL");
  assert.equal(invalidates(context, device, geometry), "");
  assert.equal(
    invalidates(context, { ...device, source: "HARDWARE" }, geometry),
    "input source changed",
  );
  assert.equal(
    invalidates(context, { ...device, state: "PAUSED" }, geometry),
    "device stopped",
  );
  assert.equal(
    invalidates(context, device, { ...geometry, width: 700 }),
    "geometry changed",
  );
});
test("blocks are separate and comparison settings preserve profile", () => {
  assert.equal(groupBlocks([{ blockId: "a" }, { blockId: "b" }]).length, 2);
  assert.deepEqual(comparisonSettings(device.profile), {
    dwellEnabled: true,
    scrollEnabled: false,
  });
  assert.equal(device.profile.gain[0], 30);
});
test("desktop and firmware simulation labels never claim live readings", () => {
  for (const source of ["SIMULATED", "FIRMWARE_SIMULATED"]) {
    const labels = sourceLabels(source);
    assert.equal(labels.synthetic, true);
    assert.match(labels.profile, /simulated/);
  }
  assert.equal(sourceLabels("HARDWARE").synthetic, false);
});
test("polls coalesce and trial persistence runs outside the command queue", async () => {
  let release;
  const calls = [];
  const client = new DeviceClient(async (endpoint) => {
    calls.push(endpoint);
    if (endpoint === "/api/device")
      await new Promise((resolve) => {
        release = resolve;
      });
    return { ok: true, json: async () => ({ ok: true }) };
  });
  const poll = client.request({ action: "status" });
  assert.equal(client.request({ action: "step" }), poll);
  await Promise.resolve();
  await client.request({ trial: 1 }, "/api/trial");
  assert.deepEqual(calls, ["/api/device", "/api/trial"]);
  release();
  await poll;
});
test("malformed acknowledgement and persistence failure are visible", async () => {
  const client = new DeviceClient(async () => ({
    ok: true,
    json: async () => ({}),
  }));
  await assert.rejects(client.request({ action: "pause" }), /Malformed/);
  const failed = new DeviceClient(async () => ({
    ok: false,
    json: async () => ({ error: "Disk full" }),
  }));
  await assert.rejects(failed.request({}, "/api/trial"), /Disk full/);
});
test("browser timeout aborts a stalled request", async () => {
  const original = globalThis.setTimeout;
  globalThis.setTimeout = (callback) => original(callback, 5);
  try {
    const client = new DeviceClient(
      (_endpoint, options) =>
        new Promise((_resolve, reject) => {
          options.signal.addEventListener("abort", () =>
            reject(Object.assign(new Error(), { name: "AbortError" })),
          );
        }),
    );
    await assert.rejects(client.request({ action: "status" }), /three seconds/);
  } finally {
    globalThis.setTimeout = original;
  }
});

test("reused block IDs cannot pool different profiles or device sources", () => {
  const base = {
    blockId: "same",
    deviceSource: "SIMULATED",
    inputSource: "SIMULATED",
    profile: device.profile,
  };
  assert.equal(
    groupBlocks([
      base,
      { ...base, deviceSource: "FIRMWARE_SIMULATED" },
      { ...base, profile: { ...device.profile, dwellEnabled: false } },
    ]).length,
    3,
  );
});

test("Lab applies matched temporary settings, freezes trials and retains persistence failures", async () => {
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
  $("condition").value = "GENERIC";
  $("inputSource").value = "SIMULATED";
  const previousDocument = globalThis.document;
  globalThis.document = { createElement: node };
  let current = { ...structuredClone(device), hasProfile: true };
  const commands = [],
    logged = [],
    messages = [];
  let exported;
  const lab = createPerformanceLab({
    $,
    geometry: () => geometry,
    getDevice: () => current,
    getPoint: () => ({ x: 400, y: 250 }),
    setPoint: () => {},
    renderCursor: () => {},
    toast: (message) => messages.push(message),
    download: (_name, text) => {
      exported = text;
    },
    action: async (action, extra) => {
      commands.push({ action, ...extra });
      if (action === "generic")
        current = {
          ...current,
          profile: {
            ...current.profile,
            dwellEnabled: false,
            scrollEnabled: true,
          },
        };
      if (action === "settings")
        current = { ...current, profile: { ...current.profile, ...extra } };
      return { ...current, ok: true };
    },
    request: async (row) => {
      logged.push(row);
      throw new Error("Disk full");
    },
  });
  try {
    await lab.start();
    assert.deepEqual(
      commands.find((command) => command.action === "settings"),
      { action: "settings", dwellEnabled: true, scrollEnabled: false },
    );
    lab.select(400, 250);
    lab.select(400, 250);
    lab.abort("test abort");
    await lab.exportTrials();
    assert.equal(logged.length, 2);
    assert.equal(logged[1].aborted, true);
    assert.equal(logged[0].blockContext.profile.dwellEnabled, true);
    assert.equal(logged[0].persisted, false);
    assert.match(exported, /persisted/);
    assert.match(messages[0], /disk logging failed/);
  } finally {
    globalThis.document = previousDocument;
  }
});
