import assert from "node:assert/strict";
import test from "node:test";

import * as api from "../src/new_dash/static/api.js";
import * as ui from "../src/new_dash/static/ui.js";
import * as live from "../src/new_dash/static/views/live.js";
import * as mapView from "../src/new_dash/static/views/map.js";

const ALL_FILTERS = {
  class: "all",
  source: "all",
  minConfidence: "all",
  freshness: "all",
};

function entity(source, displayId, overrides = {}) {
  return {
    source,
    display_id: displayId,
    class: "drone",
    confidence_pct: 90,
    stale: false,
    ...overrides,
  };
}

function state(entities, overrides = {}) {
  return {
    freshness: { state: "fresh" },
    status: { entities },
    ...overrides,
  };
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function tick() {
  return new Promise((resolve) => setImmediate(resolve));
}

test("Live groups and Map keys derive from the same filtered entity set", () => {
  const snapshot = state([
    entity("ble_rid", "RID-A", { lat: 1, lon: 2 }),
    entity("wifi_dji_ie", "DJI-A"),
  ]);
  const noMatches = { ...ALL_FILTERS, source: "wifi_ssid" };

  const groups = live.groupVisibleEntities(snapshot, noMatches);

  assert.deepEqual(
    {
      visible: groups.visible.length,
      remoteId: groups.remoteId.length,
      droneClues: groups.droneClues.length,
      other: groups.other.length,
    },
    { visible: 0, remoteId: 0, droneClues: 0, other: 0 },
  );
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, noMatches), []);
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, ALL_FILTERS), ["ble_rid:RID-A"]);
});

test("scanner summary distinguishes explicit zero from unavailable data", () => {
  assert.equal(ui.scannerSummary({ scanners: [] }), "0 scanners");
  assert.equal(ui.scannerSummary({}), "Scanners unavailable");
  assert.equal(ui.scannerSummary({ scanners: null }), "Scanners unavailable");
  assert.equal(ui.scannerSummary({ scanners: "bad" }), "Scanners unavailable");
  assert.equal(
    ui.scannerSummary({ scanners: [{}, {}], sensing_health: "safe_usb" }),
    "2 scanners · safe usb",
  );
});

test("tab key navigation supports an uninterrupted Map, History, Badge, Live sequence", () => {
  let index = 1;
  index = ui.nextTabIndex(index, "ArrowRight", 4);
  assert.equal(index, 2);
  index = ui.nextTabIndex(index, "ArrowRight", 4);
  assert.equal(index, 3);
  index = ui.nextTabIndex(index, "Home", 4);
  assert.equal(index, 0);
  assert.equal(ui.nextTabIndex(index, "ArrowLeft", 4), 3);
  assert.equal(ui.nextTabIndex(index, "Enter", 4), null);
});

test("external request abort cancels the active fetch and removes its listener", async () => {
  const originalWindow = globalThis.window;
  const originalFetch = globalThis.fetch;
  const controller = new AbortController();
  const signal = controller.signal;
  const originalAdd = signal.addEventListener.bind(signal);
  const originalRemove = signal.removeEventListener.bind(signal);
  let additions = 0;
  let removals = 0;
  let fetchSignal;
  signal.addEventListener = (...args) => {
    additions += 1;
    return originalAdd(...args);
  };
  signal.removeEventListener = (...args) => {
    removals += 1;
    return originalRemove(...args);
  };
  globalThis.window = globalThis;
  globalThis.fetch = (_path, options) => {
    fetchSignal = options.signal;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        resolve({
          ok: true,
          status: 200,
          json: async () => ({ ok: true, data: { value: "late" } }),
        });
      }, 15);
      fetchSignal.addEventListener("abort", () => {
        clearTimeout(timer);
        const error = new Error("aborted");
        error.name = "AbortError";
        reject(error);
      }, { once: true });
    });
  };

  try {
    const request = api.getState({ signal });
    controller.abort();
    const outcome = await request.catch((error) => error);

    assert.equal(fetchSignal.aborted, true);
    assert.equal(outcome.code, "request_aborted");
    assert.equal(additions, 1);
    assert.equal(removals, 1);
  } finally {
    signal.addEventListener = originalAdd;
    signal.removeEventListener = originalRemove;
    globalThis.window = originalWindow;
    globalThis.fetch = originalFetch;
  }
});

test("completion poller aborts teardown, never reschedules it, and resumes once", async () => {
  const requests = [];
  const timers = [];
  const values = [];
  const poller = api.createCompletionPoller({
    load: (signal) => {
      const pending = deferred();
      requests.push({ signal, ...pending });
      signal.addEventListener("abort", () => {
        const error = new Error("aborted");
        error.name = "AbortError";
        pending.reject(error);
      }, { once: true });
      return pending.promise;
    },
    onValue: (value) => values.push(value),
    onError: (error) => assert.fail(`abort reached onError: ${error}`),
    schedule: (callback, delay) => {
      const timer = { callback, delay, cancelled: false };
      timers.push(timer);
      return timer;
    },
    cancel: (timer) => {
      timer.cancelled = true;
    },
    intervalMs: 1000,
  });

  poller.start();
  assert.equal(requests.length, 1);
  poller.stop();
  await tick();
  assert.equal(requests[0].signal.aborted, true);
  assert.equal(timers.length, 0);

  poller.start();
  poller.start();
  assert.equal(requests.length, 2);
  requests[1].resolve("resumed");
  await tick();
  assert.deepEqual(values, ["resumed"]);
  assert.equal(timers.length, 1);
  assert.equal(timers[0].delay, 1000);

  timers[0].callback();
  assert.equal(requests.length, 3);
  poller.stop();
  await tick();
  assert.equal(timers.length, 1);
});

test("trail generations clear old rows and obsolete responses cannot repaint", async () => {
  const pending = new Map();
  const renders = [];
  const controller = mapView.createTrailController({
    getHistory: (query) => {
      const key = query.get("text");
      const request = deferred();
      pending.set(key, request);
      return request.promise;
    },
    render: (rows) => renders.push(rows.map((row) => row.stable_key)),
    reportError: assert.fail,
  });

  const oldGeneration = controller.update(["ble_rid:OLD"]);
  const newGeneration = controller.update(["wifi_rid:NEW"]);
  assert.deepEqual(renders, [[], []]);

  pending.get("ble_rid:OLD").resolve({
    items: [{ stable_key: "ble_rid:OLD", latitude: 1, longitude: 2 }],
    next_cursor: null,
  });
  await oldGeneration;
  assert.deepEqual(renders, [[], []]);

  pending.get("wifi_rid:NEW").resolve({
    items: [
      { stable_key: "wifi_rid:NEW", latitude: 3, longitude: 4 },
      { stable_key: "ble_rid:OLD", latitude: 1, longitude: 2 },
    ],
    next_cursor: null,
  });
  await newGeneration;
  assert.deepEqual(renders.at(-1), ["wifi_rid:NEW"]);

  await controller.update([]);
  assert.deepEqual(renders.at(-1), []);
});

test("trail generation retries one transient failure without latching forever", async () => {
  let attempts = 0;
  const renders = [];
  const errors = [];
  const controller = mapView.createTrailController({
    getHistory: async () => {
      attempts += 1;
      if (attempts === 1) {
        throw new Error("temporary");
      }
      return {
        items: [{ stable_key: "ble_rid:A", latitude: 1, longitude: 2 }],
        next_cursor: null,
      };
    },
    render: (rows) => renders.push(rows),
    reportError: (error) => errors.push(error.message),
  });

  await controller.update(["ble_rid:A"]);
  assert.equal(attempts, 1);
  assert.deepEqual(errors, ["temporary"]);

  await controller.update(["ble_rid:A"]);
  assert.equal(attempts, 2);
  assert.equal(renders.at(-1)[0].stable_key, "ble_rid:A");

  await controller.update(["ble_rid:A"]);
  assert.equal(attempts, 2);
});

test("one trail generation shares a four-page and 2000-row global budget", async () => {
  const calls = [];
  const rendered = [];
  const controller = mapView.createTrailController({
    getHistory: async (query) => {
      calls.push(query.toString());
      const key = query.get("text");
      const cursor = query.get("cursor") || "first";
      return {
        items: Array.from({ length: 500 }, (_, index) => ({
          stable_key: key,
          latitude: 1,
          longitude: index / 1000,
        })),
        next_cursor: `${key}:${cursor}:next`,
      };
    },
    render: (rows) => rendered.push(rows),
    reportError: assert.fail,
  });

  await controller.update(["wifi_rid:B", "ble_rid:A"]);

  assert.equal(calls.length, 4);
  assert.equal(rendered.at(-1).length, 2000);
  assert.ok(calls.every((query) => query.includes("kind=track")));
  assert.ok(calls.every((query) => query.includes("positioned=true")));
  assert.ok(calls.every((query) => query.includes("limit=500")));
});
