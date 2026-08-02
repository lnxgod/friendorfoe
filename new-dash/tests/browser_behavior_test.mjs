import assert from "node:assert/strict";
import test from "node:test";

import * as api from "../src/new_dash/static/api.js";
import * as ui from "../src/new_dash/static/ui.js";
import * as live from "../src/new_dash/static/views/live.js";
import * as mapView from "../src/new_dash/static/views/map.js";
import * as historyView from "../src/new_dash/static/views/history.js";
import * as badgeView from "../src/new_dash/static/views/badge.js";

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

test("History fetches on activation and actions, not status polling", async () => {
  const calls = [];
  const controller = historyView.createHistoryController({
    getHistory: async (query) => {
      calls.push(query.toString());
      return { items: [{ row_id: calls.length }], next_cursor: "opaque-next" };
    },
  });

  await controller.activate(true);
  controller.observeAvailability(true);
  controller.observeAvailability(true);
  assert.equal(calls.length, 1);

  await controller.applyFilters({
    since: "1700000000", until: "1700000300", kind: "event",
    class: "drone", source: "ble_rid", text: "RID & <hostile>", positioned: true,
  });
  assert.equal(
    calls.at(-1),
    "since=1700000000&until=1700000300&kind=event&class=drone&source=ble_rid&text=RID+%26+%3Chostile%3E&positioned=true&limit=25",
  );
  assert.deepEqual(controller.snapshot().cursorStack, []);
});

test("History uses an opaque cursor stack and exports only active filters", async () => {
  const calls = [];
  const pages = [
    { items: [{ row_id: 30 }], next_cursor: "opaque:A/+=?" },
    { items: [{ row_id: 29 }], next_cursor: "opaque:B" },
    { items: [{ row_id: 30 }], next_cursor: "opaque:A/+=?" },
  ];
  const controller = historyView.createHistoryController({
    getHistory: async (query) => {
      calls.push(query.toString());
      return pages.shift();
    },
  });
  await controller.activate(true);
  await controller.next();
  assert.match(calls.at(-1), /cursor=opaque%3AA%2F%2B%3D%3F/);
  assert.deepEqual(controller.snapshot().cursorStack, [null]);
  await controller.previous();
  assert.equal(controller.snapshot().cursor, null);
  assert.deepEqual(controller.snapshot().cursorStack, []);

  await controller.applyFilters({ source: "wifi_rid", text: "RID 7", positioned: false });
  const exports = controller.exportLinks();
  assert.equal(exports.csv, "/api/history/export.csv?source=wifi_rid&text=RID+7&positioned=false");
  assert.equal(exports.json, "/api/history/export.json?source=wifi_rid&text=RID+7&positioned=false");
  for (const href of Object.values(exports)) {
    assert.doesNotMatch(href, /cursor|limit|token/i);
  }
});

test("History clear requires exact typed confirmation and resets pagination", async () => {
  const posts = [];
  let fetches = 0;
  const controller = historyView.createHistoryController({
    getHistory: async () => {
      fetches += 1;
      return { items: [{ row_id: fetches }], next_cursor: fetches === 1 ? "next" : null };
    },
    post: async (path, body) => {
      posts.push([path, body]);
      return { deleted: 25 };
    },
  });
  await controller.activate(true);
  await controller.next();
  assert.equal(await controller.clear("clear"), false);
  assert.equal(await controller.clear("CLEAR "), false);
  assert.deepEqual(posts, []);
  assert.equal(await controller.clear("CLEAR"), true);
  assert.deepEqual(posts, [["/api/history/clear", { confirm: "clear-history" }]]);
  assert.equal(controller.snapshot().cursor, null);
  assert.deepEqual(controller.snapshot().cursorStack, []);
});

test("History state distinguishes loading, empty, API error, and unavailable", async () => {
  const pending = deferred();
  const controller = historyView.createHistoryController({ getHistory: () => pending.promise });
  const activation = controller.activate(true);
  assert.equal(controller.snapshot().phase, "loading");
  pending.resolve({ items: [], next_cursor: null });
  await activation;
  assert.equal(controller.snapshot().phase, "empty");

  const failing = historyView.createHistoryController({
    getHistory: async () => { throw new Error("database offline"); },
  });
  await failing.activate(true);
  assert.equal(failing.snapshot().phase, "error");
  failing.observeAvailability(false, "storage unavailable");
  assert.equal(failing.snapshot().phase, "unavailable");
});

test("Dialog focus wraps and Escape/cancel never confirms", () => {
  const first = { disabled: false };
  const middle = { disabled: false };
  const last = { disabled: false };
  assert.equal(historyView.nextDialogFocus([first, middle, last], last, false), first);
  assert.equal(historyView.nextDialogFocus([first, middle, last], first, true), last);
  assert.equal(historyView.nextDialogFocus([first, { disabled: true }, last], first, false), last);
});

test("Badge draft polling preserves edits and waits for matching firmware status", () => {
  const firmware = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: 1, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  const edited = structuredClone(firmware);
  edited.brightness = 70;
  const draft = badgeView.createDraftState(badgeView.validateTheme);
  draft.observe(firmware);
  draft.edit(edited);
  draft.observe({ ...firmware, brightness: 90 });
  assert.equal(draft.snapshot().draft.brightness, 70);
  draft.accept();
  assert.equal(badgeView.applyAllowed({
    canMutate: true, pending: false, draft: draft.snapshot(),
  }), false);
  draft.observe(firmware);
  assert.equal(draft.snapshot().dirty, true);
  draft.observe(edited);
  assert.equal(draft.snapshot().dirty, false);
});

test("Badge incomplete objects disable Apply and validation sends no request", async () => {
  const posts = [];
  const mutations = badgeView.createMutationCoordinator({
    post: async (...args) => { posts.push(args); return { ok: true }; },
  });
  assert.equal(badgeView.validateTheme({ version: 1 }).ok, false);
  assert.equal(badgeView.validatePolicy({ version: 1, classes: {} }).ok, false);
  const result = await mutations.submitValidated(
    "/api/control/theme", { version: 1 }, badgeView.validateTheme,
  );
  assert.equal(result.accepted, false);
  assert.deepEqual(posts, []);
});

test("Badge permits one global pending command and accepted replies stay non-optimistic", async () => {
  const pending = deferred();
  const posts = [];
  const mutations = badgeView.createMutationCoordinator({
    post: (...args) => { posts.push(args); return pending.promise; },
  });
  const first = mutations.submit("/api/control/display-nav", { action: "next" });
  const second = await mutations.submit("/api/control/display-nav", { action: "back" });
  assert.equal(second.accepted, false);
  assert.equal(posts.length, 1);
  pending.resolve({ ok: true, message: "display nav updated", ble_sent: true, wifi_sent: false });
  const reply = await first;
  assert.equal(reply.message, "Accepted; awaiting status · BLE sent · Wi-Fi not sent");
  assert.equal(mutations.snapshot().pending, false);
});

test("Badge rejected replies preserve draft and zero diagnostics are not missing", async () => {
  const draft = badgeView.createDraftState(badgeView.validateTheme);
  const current = {
    version: 1, palette: "mono", background: "dim", brightness: 25,
    accents: { drone: 0, meta: 0, tracker: 0, flock: 0, wifi_attack: 0, clear: 0 },
  };
  draft.observe(current);
  draft.edit({ ...current, brightness: 26 });
  const mutations = badgeView.createMutationCoordinator({
    post: async () => { throw new Error("firmware rejected"); },
  });
  const reply = await mutations.submit("/api/control/theme", draft.snapshot().draft);
  assert.equal(reply.accepted, false);
  assert.equal(draft.snapshot().draft.brightness, 26);
  assert.equal(badgeView.presentFact({ commands_failed: 0 }, "commands_failed"), "0");
  assert.equal(badgeView.presentFact({}, "commands_failed"), null);
});

test("Badge emits exact navigation and complete policy payloads", async () => {
  const posts = [];
  const mutations = badgeView.createMutationCoordinator({
    post: async (path, body) => { posts.push([path, body]); return { ok: true }; },
  });
  await mutations.submit("/api/control/display-nav", badgeView.navigationPayload("detail"));
  assert.deepEqual(posts[0], ["/api/control/display-nav", { action: "detail" }]);
  assert.throws(() => badgeView.navigationPayload("reboot"));

  const classes = Object.fromEntries(badgeView.POLICY_CLASSES.map((name) => [name, {
    enabled: true, lane: "lower", min_proximity: "near", priority: 0,
  }]));
  const policy = { version: 1, classes };
  assert.equal(badgeView.validatePolicy(policy).ok, true);
  await mutations.submitValidated("/api/control/display-policy", policy, badgeView.validatePolicy);
  assert.deepEqual(posts[1], ["/api/control/display-policy", policy]);
});
