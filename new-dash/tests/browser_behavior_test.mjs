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

function completePolicy() {
  const row = (priority) => ({
    enabled: true, lane: "lower", min_proximity: "near", priority,
  });
  return {
    version: 1,
    classes: {
      drone: row(0), meta: row(1), tracker: row(2), wifi_attack: row(3),
      skimmer: row(4), camera: row(5), flock: row(6), lock: row(7),
      hid: row(8), beacon: row(9), event_badge: row(10), auracast: row(11),
      scanner_status: row(12),
    },
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

test("Map can retain 200 GPS identities while Live stays current and markers fade", () => {
  const retained = Array.from({ length: 200 }, (_, index) => entity(
    "ble_rid",
    `SIM-${String(index).padStart(3, "0")}`,
    {
      stable_key: `ble_rid:SIM-${String(index).padStart(3, "0")}`,
      lat: 37 + index / 100_000,
      lon: -120 - index / 100_000,
      host_age_s: index % 120,
      position_retention_s: 120,
      host_retained: index !== 199,
    },
  ));
  const snapshot = state([retained.at(-1)], {
    positioned_remote_id_entities: retained,
  });

  assert.equal(live.groupVisibleEntities(snapshot, ALL_FILTERS).remoteId.length, 1);
  assert.equal(live.visiblePositionedRemoteIds(snapshot, ALL_FILTERS).length, 200);
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, ALL_FILTERS), ["ble_rid:SIM-199"]);
  assert.equal(ui.stableEntityKey(retained[0]), "ble_rid:SIM-000");
  assert.equal(mapView.positionOpacity({ host_age_s: 0, position_retention_s: 120 }), 1);
  assert.equal(mapView.positionOpacity({ host_age_s: 60, position_retention_s: 120 }), 0.6);
  assert.equal(mapView.positionOpacity({ host_age_s: 120, position_retention_s: 120 }), 0.2);
});

test("Live presents deduplicated native USB detections separately when active entities are unavailable", () => {
  const snapshot = {
    freshness: { state: "fresh" },
    status: { version: "0.67.2", recovery_mode: "startup_dependency" },
    recent_events: [
      {
        detection_id: "rid_FOF-SIM-001",
        badge_label: "Remote ID",
        badge_class: "drone",
        badge_entity_key: "DRONE:rid_FOF-SIM-001",
        source: "ble_rid",
        confidence: 0.9,
        threat_score: 97,
        rssi: -49,
        received_at: 102,
      },
      {
        detection_id: "rid_FOF-SIM-001",
        badge_label: "Older Remote ID label",
        badge_class: "drone",
        badge_entity_key: "DRONE:rid_FOF-SIM-001",
        source: "ble_rid",
        confidence: 0.1,
        threat_score: 12,
        rssi: -80,
        received_at: 101,
      },
      {
        detection_id: "BLE:17D27B12:FindMy Accessory",
        manufacturer: "Find My",
        badge_class: "tracker",
        source: "ble_fingerprint",
        confidence: 65,
        threat_score: 36,
        rssi: -52,
        received_at: 100,
      },
      {
        detection_id: "BLE:17D27B12:FindMy Accessory",
        manufacturer: "Older Find My label",
        badge_class: "tracker",
        source: "ble_fingerprint",
        confidence: 0.2,
        threat_score: 10,
        rssi: -90,
        received_at: 99,
      },
      {
        badge_entity_key: "META:key-only",
        badge_label: "Badge-key-only event",
        badge_class: "meta",
        source: "ble_fingerprint",
        confidence: 0.5,
        threat_score: 20,
        rssi: -60,
        received_at: 98,
      },
      null,
      [],
      { source: "ble_rid" },
    ],
  };

  const recent = live.recentUsbDetections(snapshot, ALL_FILTERS, 105);

  assert.deepEqual(recent, [
    {
      display_id: "rid_FOF-SIM-001",
      label: "Remote ID",
      class: "drone",
      source: "ble_rid",
      confidence_pct: 90,
      score: 97,
      rssi: -49,
      last_seen_s: 3,
      evidence: "Recent native USB detection: Remote ID.",
      recent_native_usb: true,
    },
    {
      display_id: "BLE:17D27B12:FindMy Accessory",
      label: "Find My",
      class: "tracker",
      source: "ble_fingerprint",
      confidence_pct: 65,
      score: 36,
      rssi: -52,
      last_seen_s: 5,
      evidence: "Recent native USB detection: Find My.",
      recent_native_usb: true,
    },
    {
      display_id: null,
      label: "Badge-key-only event",
      class: "meta",
      source: "ble_fingerprint",
      confidence_pct: 50,
      score: 20,
      rssi: -60,
      last_seen_s: 7,
      evidence: "Recent native USB detection: Badge-key-only event.",
      recent_native_usb: true,
    },
  ]);
  assert.deepEqual(live.groupVisibleEntities(snapshot, ALL_FILTERS).visible, []);
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, ALL_FILTERS), []);
  assert.equal(
    live.stateBanners(snapshot).some(([message, tone]) => (
      tone === "info"
      && message === "Active badge entity snapshot is temporarily unavailable. Recent native USB detections are shown separately."
    )),
    true,
  );
});

test("missing active entities are called unavailable while New Dash waits for native USB detections", () => {
  const snapshot = {
    freshness: { state: "fresh" },
    status: { version: "0.67.2", recovery_mode: "startup_dependency" },
    recent_events: [],
  };

  assert.deepEqual(live.recentUsbDetections(snapshot, ALL_FILTERS, 101), []);
  assert.equal(
    live.stateBanners(snapshot).some(([message, tone]) => (
      tone === "info"
      && message === "Active badge entity snapshot is temporarily unavailable. Waiting for native USB detections."
    )),
    true,
  );
});

test("unavailable active sections do not present a numeric empty count", () => {
  assert.equal(live.sectionCountLabel(0, true), "Unavailable");
  assert.equal(live.sectionCountLabel(0), "0 items");
  assert.equal(live.sectionCountLabel(1), "1 item");
});

test("recent native USB detections honor presentation filters", () => {
  const snapshot = {
    freshness: { state: "fresh" },
    status: {},
    recent_events: [
      {
        detection_id: "RID-A",
        badge_label: "Remote ID",
        badge_class: "drone",
        source: "ble_rid",
        confidence: 0.9,
        received_at: 100,
      },
      {
        detection_id: "TAG-A",
        badge_label: "Find My",
        badge_class: "tracker",
        source: "ble_fingerprint",
        confidence: 0.65,
        received_at: 99,
      },
    ],
  };

  assert.deepEqual(
    live.recentUsbDetections(snapshot, { ...ALL_FILTERS, class: "tracker" }, 101)
      .map((item) => item.display_id),
    ["TAG-A"],
  );
  assert.deepEqual(
    live.recentUsbDetections(snapshot, {
      ...ALL_FILTERS,
      source: "ble_rid",
      minConfidence: "95",
    }, 101),
    [],
  );
  assert.deepEqual(
    live.recentUsbDetections(snapshot, { ...ALL_FILTERS, freshness: "stale" }, 101),
    [],
  );
});

test("an explicit empty active entity snapshot remains authoritative beside recent USB detections", () => {
  const snapshot = state([], {
    status: { version: "0.67.2", entities: [] },
    recent_events: [{
      detection_id: "RID-A",
      badge_label: "Remote ID",
      badge_class: "drone",
      source: "ble_rid",
      confidence: 0.9,
      received_at: 100,
    }],
  });

  assert.deepEqual(live.visibleEntities(snapshot, ALL_FILTERS), []);
  assert.deepEqual(live.groupVisibleEntities(snapshot, ALL_FILTERS).visible, []);
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, ALL_FILTERS), []);
  assert.deepEqual(
    live.recentUsbDetections(snapshot, ALL_FILTERS, 101).map((item) => item.display_id),
    ["RID-A"],
  );
  assert.equal(
    live.stateBanners(snapshot)
      .some(([message]) => message.includes("Active badge entity snapshot is temporarily unavailable")),
    false,
  );
});

test("Other class includes every class outside the four named filters for Live and Map", () => {
  const snapshot = state([
    entity("wifi_ssid", "FLOCK", { class: "flock" }),
    entity("wifi_ssid", "SKIMMER", { class: "skimmer" }),
    entity("wifi_ssid", "LITERAL", { class: "other" }),
    entity("wifi_ssid", "MISSING", { class: null }),
    entity("ble_rid", "RID-OTHER", { class: "auracast", lat: 1, lon: 2 }),
    entity("wifi_ssid", "META", { class: "meta" }),
    entity("wifi_ssid", "ATTACK", { class: "wifi_attack" }),
  ]);
  const otherFilters = { ...ALL_FILTERS, class: "other" };

  const groups = live.groupVisibleEntities(snapshot, otherFilters);

  assert.deepEqual(
    groups.visible.map((item) => item.display_id),
    ["FLOCK", "SKIMMER", "LITERAL", "MISSING", "RID-OTHER"],
  );
  assert.deepEqual(live.filteredRemoteIdKeys(snapshot, otherFilters), ["ble_rid:RID-OTHER"]);
});

test("Live keeps local history failures and observation drops visible", () => {
  const dropped = state([], { diagnostics: { persistence_drops: 3 } });
  const unavailable = state([], {
    diagnostics: {
      history_available: false,
      history_error: "Private database path and exception details",
      persistence_drops: 0,
    },
  });

  assert.deepEqual(
    live.stateBanners(dropped).filter(([message]) => message.includes("could not be saved")),
    [["Local history is incomplete: 3 observations could not be saved.", "danger"]],
  );
  assert.deepEqual(
    live.stateBanners(unavailable).filter(([message]) => message.includes("not being saved")),
    [["Local history is unavailable; observations are not being saved.", "danger"]],
  );
  assert.equal(
    live.stateBanners(unavailable)
      .some(([message]) => message.includes("Private database path")),
    false,
  );
  assert.equal(
    live.stateBanners(state([], { diagnostics: { persistence_drops: 0 } }))
      .some(([message]) => message.includes("could not be saved")),
    false,
  );
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

test("Map reactivation refetches unchanged trails once without poll refetches", async () => {
  let calls = 0;
  const controller = mapView.createTrailController({
    getHistory: async () => {
      calls += 1;
      return {
        items: [{ stable_key: "ble_rid:A", latitude: 1, longitude: 2 }],
        next_cursor: null,
      };
    },
    render: () => {},
    reportError: assert.fail,
  });

  await controller.refresh(["ble_rid:A"]);
  await controller.update(["ble_rid:A"]);
  await controller.update(["ble_rid:A"]);
  assert.equal(calls, 1);

  await controller.refresh(["ble_rid:A"]);
  await controller.update(["ble_rid:A"]);
  assert.equal(calls, 2);
});

test("trail warning survives state success and retry exhaustion until a trail reset", async () => {
  let calls = 0;
  const recovery = deferred();
  const renderedStatuses = [];
  const statuses = mapView.createRequestStatusChannels({
    render: (message) => renderedStatuses.push(message),
  });
  const controller = mapView.createTrailController({
    getHistory: () => {
      calls += 1;
      if (calls === 1) throw new Error("temporary trail failure");
      if (calls === 2) throw new Error("trail still unavailable");
      return recovery.promise;
    },
    render: () => {},
    reportError: (error) => statuses.setTrail(
      `Host-observed trail unavailable: ${error.message}`,
    ),
    clearError: () => statuses.clearTrail(),
  });

  await controller.update(["ble_rid:A"]);
  statuses.setState("State request failed.");
  statuses.clearState();
  assert.equal(
    renderedStatuses.at(-1),
    "Host-observed trail unavailable: temporary trail failure",
  );

  await controller.update(["ble_rid:A"]);
  await controller.update(["ble_rid:A"]);
  statuses.clearState();
  assert.equal(calls, 2);
  assert.equal(
    renderedStatuses.at(-1),
    "Host-observed trail unavailable: trail still unavailable",
  );

  statuses.setState("State request failed.");
  const reload = controller.refresh(["ble_rid:A"]);
  assert.equal(renderedStatuses.at(-1), "State request failed.");
  recovery.resolve({ items: [], next_cursor: null });
  await reload;
  assert.equal(renderedStatuses.at(-1), "State request failed.");
  statuses.clearState();
  assert.equal(renderedStatuses.at(-1), "");
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

test("History failed Next keeps the current cursor stack retryable", async () => {
  let request = 0;
  const controller = historyView.createHistoryController({
    getHistory: async () => {
      request += 1;
      if (request === 1) return { items: [{ row_id: 30 }], next_cursor: "cursor-a" };
      if (request === 2) throw new Error("transient next failure");
      return { items: [{ row_id: 29 }], next_cursor: "cursor-b" };
    },
  });

  await controller.activate(true);
  assert.equal(await controller.next(), false);
  assert.equal(controller.snapshot().cursor, null);
  assert.deepEqual(controller.snapshot().cursorStack, []);
  assert.equal(controller.snapshot().nextCursor, "cursor-a");

  assert.equal(await controller.next(), true);
  assert.equal(controller.snapshot().cursor, "cursor-a");
  assert.deepEqual(controller.snapshot().cursorStack, [null]);
});

test("History failed Previous preserves the current page and back stack", async () => {
  let request = 0;
  const controller = historyView.createHistoryController({
    getHistory: async () => {
      request += 1;
      if (request === 1) return { items: [{ row_id: 30 }], next_cursor: "cursor-a" };
      if (request === 2) return { items: [{ row_id: 29 }], next_cursor: "cursor-b" };
      if (request === 3) throw new Error("transient previous failure");
      return { items: [{ row_id: 30 }], next_cursor: "cursor-a" };
    },
  });

  await controller.activate(true);
  await controller.next();
  assert.equal(await controller.previous(), false);
  assert.equal(controller.snapshot().cursor, "cursor-a");
  assert.deepEqual(controller.snapshot().cursorStack, [null]);
  assert.equal(controller.snapshot().nextCursor, "cursor-b");

  assert.equal(await controller.previous(), true);
  assert.equal(controller.snapshot().cursor, null);
  assert.deepEqual(controller.snapshot().cursorStack, []);
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

test("History active storage recovery loads exactly once on unavailable to available", async () => {
  let calls = 0;
  const controller = historyView.createHistoryController({
    getHistory: async () => {
      calls += 1;
      return { items: [{ row_id: 1 }], next_cursor: null };
    },
  });

  await controller.activate(false, "storage unavailable");
  assert.equal(calls, 0);
  await controller.observeAvailability(true);
  await controller.observeAvailability(true);
  assert.equal(calls, 1);
  assert.equal(controller.snapshot().phase, "ready");
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

test("Badge display controls hide only for the exact trusted headless Lite identity", () => {
  const lite = {
    product_family: "badge_lite",
    target: "uplink-s3-backend",
    project: "fof_backend_uplink",
    hardware: "seeed_xiao_esp32s3",
    mode: "headless",
    capabilities: ["display_none", "usb_live", "usb_live_ack"],
  };

  assert.equal(badgeView.isTrustedHeadlessLite(lite), true);
  assert.equal(badgeView.isTrustedHeadlessLite({ ...lite, hardware: "other_s3" }), false);
  assert.equal(badgeView.isTrustedHeadlessLite({
    ...lite, capabilities: ["usb_live", "usb_live_ack"],
  }), false);
  assert.equal(badgeView.isTrustedHeadlessLite({
    target: "uplink-s3-fof_badge",
    project: "fof_badge_uplink",
    capabilities: [],
  }), false);
});

test("Badge restoring reordered exact current settings makes the draft pristine", () => {
  const current = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: 1, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  const reorderedCurrent = {
    accents: { clear: 6, wifi_attack: 5, flock: 4, tracker: 3, meta: 2, drone: 1 },
    brightness: 80, background: "dark", palette: "night", version: 1,
  };
  const draft = badgeView.createDraftState(badgeView.validateTheme);

  draft.observe(current);
  draft.edit({ ...current, brightness: 70 });
  assert.equal(draft.snapshot().dirty, true);
  draft.edit(reorderedCurrent);
  assert.equal(draft.snapshot().dirty, false);
  assert.equal(badgeView.applyAllowed({
    canMutate: true, pending: false, draft: draft.snapshot(),
  }), false);
});

test("Badge policy equality ignores class and field key order", () => {
  const current = completePolicy();
  const reordered = {
    classes: Object.fromEntries(Object.entries(current.classes).reverse().map(([name, policy]) => [
      name,
      {
        priority: policy.priority,
        min_proximity: policy.min_proximity,
        lane: policy.lane,
        enabled: policy.enabled,
      },
    ])),
    version: 1,
  };
  const draft = badgeView.createDraftState(badgeView.validatePolicy);

  draft.observe(current);
  draft.edit(reordered);
  assert.equal(draft.snapshot().valid, true);
  assert.equal(draft.snapshot().dirty, false);
});

test("Badge accepted reordered draft becomes confirmed on schema-equal status", async () => {
  const current = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: 1, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  const submitted = {
    accents: { clear: 60, wifi_attack: 50, flock: 40, tracker: 30, meta: 20, drone: 10 },
    brightness: 70, background: "dim", palette: "field", version: 1,
  };
  const confirmedStatus = {
    palette: "field", version: 1,
    accents: { meta: 20, drone: 10, clear: 60, tracker: 30, wifi_attack: 50, flock: 40 },
    background: "dim", brightness: 70,
  };
  const draft = badgeView.createDraftState(badgeView.validateTheme);
  const mutations = badgeView.createMutationCoordinator({
    post: async () => ({ ok: true, ble_sent: true, wifi_sent: true }),
  });

  draft.observe(current);
  draft.edit(submitted);
  const reply = await mutations.submitValidated(
    "/api/control/theme", draft.snapshot().draft, badgeView.validateTheme,
  );
  assert.equal(reply.accepted, true);
  draft.accept();
  assert.match(mutations.snapshot().message, /awaiting status/);

  assert.equal(typeof badgeView.observeDraftStatus, "function");
  const observation = badgeView.observeDraftStatus({
    draft, mutations, path: "/api/control/theme", label: "Theme", value: confirmedStatus,
  });
  assert.equal(observation.confirmed, true);
  assert.equal(mutations.snapshot().message, "Theme confirmed by firmware status.");
  assert.equal(mutations.snapshot().awaitingConfirmation, null);
  assert.equal(draft.snapshot().dirty, false);
  assert.equal(draft.snapshot().awaiting, null);
});

test("Badge status before deferred acceptance confirms canonical theme and policy submissions", async () => {
  const themeCurrent = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: 1, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  const themeSubmitted = {
    accents: { clear: 60, wifi_attack: 50, flock: 40, tracker: 30, meta: 20, drone: 10 },
    brightness: 70, background: "dim", palette: "field", version: 1,
  };
  const themeStatus = {
    palette: "field", version: 1,
    accents: { meta: 20, drone: 10, clear: 60, tracker: 30, wifi_attack: 50, flock: 40 },
    background: "dim", brightness: 70,
  };
  const policyCurrent = completePolicy();
  const policySubmitted = structuredClone(policyCurrent);
  policySubmitted.classes.drone.priority = 99;
  const policyStatus = {
    classes: Object.fromEntries(Object.entries(policySubmitted.classes).reverse().map(([name, row]) => [
      name,
      {
        priority: row.priority, min_proximity: row.min_proximity,
        lane: row.lane, enabled: row.enabled,
      },
    ])),
    version: 1,
  };
  const policyNewerDraft = structuredClone(policySubmitted);
  policyNewerDraft.classes.drone.priority = 88;
  const cases = [
    {
      name: "theme", path: "/api/control/theme", label: "Theme",
      validate: badgeView.validateTheme, current: themeCurrent,
      submitted: themeSubmitted, status: themeStatus,
      readCurrent: (snapshot) => snapshot.current.brightness,
      readDraft: (snapshot) => snapshot.draft.brightness,
      before: 80, confirmed: 70, finalDraft: 70,
    },
    {
      name: "policy", path: "/api/control/display-policy", label: "Display policy",
      validate: badgeView.validatePolicy, current: policyCurrent,
      submitted: policySubmitted, status: policyStatus, newerDraft: policyNewerDraft,
      readCurrent: (snapshot) => snapshot.current.classes.drone.priority,
      readDraft: (snapshot) => snapshot.draft.classes.drone.priority,
      before: 0, confirmed: 99, finalDraft: 88,
    },
  ];

  for (const scenario of cases) {
    const pending = deferred();
    const draft = badgeView.createDraftState(scenario.validate);
    const mutations = badgeView.createMutationCoordinator({ post: () => pending.promise });
    draft.observe(scenario.current);
    draft.edit(scenario.submitted);

    const command = badgeView.submitDraft({
      draft, mutations, path: scenario.path, label: scenario.label,
      validate: scenario.validate,
    });
    assert.equal(mutations.snapshot().pending, true, `${scenario.name} POST is pending`);
    assert.equal(
      scenario.readCurrent(draft.snapshot()), scenario.before,
      `${scenario.name} submission must not optimistically mutate current status`,
    );
    if (scenario.newerDraft) draft.edit(scenario.newerDraft);

    const observation = badgeView.observeDraftStatus({
      draft, mutations, path: scenario.path, label: scenario.label, value: scenario.status,
    });
    assert.equal(observation.confirmed, false, `${scenario.name} cannot confirm before acceptance`);
    assert.equal(scenario.readCurrent(draft.snapshot()), scenario.confirmed);
    const observedCurrent = structuredClone(draft.snapshot().current);

    pending.resolve({ ok: true, ble_sent: true, wifi_sent: true });
    const reply = await command;
    assert.equal(reply.accepted, true);
    assert.deepEqual(
      draft.snapshot().current, observedCurrent,
      `${scenario.name} acceptance must not optimistically mutate current status`,
    );
    assert.equal(
      mutations.snapshot().message,
      `${scenario.label} confirmed by firmware status.`,
    );
    assert.equal(mutations.snapshot().awaitingConfirmation, null);
    assert.equal(draft.snapshot().awaiting, null);
    assert.equal(scenario.readDraft(draft.snapshot()), scenario.finalDraft);
    assert.equal(draft.snapshot().dirty, Boolean(scenario.newerDraft));
  }
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

test("Badge DOM numeric reads keep cleared accent and priority fields invalid", () => {
  assert.equal(typeof badgeView.readRequiredInteger, "function");
  assert.equal(badgeView.readRequiredInteger({ value: "" }), null);
  assert.equal(badgeView.readRequiredInteger({ value: "0" }), 0);
  assert.equal(badgeView.readRequiredInteger({ value: "42" }), 42);

  const theme = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: null, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  assert.equal(badgeView.validateTheme(theme).ok, false);
  const policy = completePolicy();
  policy.classes.drone.priority = null;
  assert.equal(badgeView.validatePolicy(policy).ok, false);
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

test("Badge current snapshots stay complete while rejected drafts remain separate", () => {
  assert.equal(typeof badgeView.themeSnapshotFacts, "function");
  assert.equal(typeof badgeView.policySnapshotRows, "function");
  const currentTheme = {
    version: 1, palette: "night", background: "dark", brightness: 80,
    accents: { drone: 1, meta: 2, tracker: 3, flock: 4, wifi_attack: 5, clear: 6 },
  };
  const themeDraft = badgeView.createDraftState(badgeView.validateTheme);
  themeDraft.observe(currentTheme);
  themeDraft.edit({ ...currentTheme, brightness: 70 });
  assert.deepEqual(badgeView.themeSnapshotFacts(themeDraft.snapshot().current), [
    ["Version", "1"], ["Palette", "night"], ["Background", "dark"],
    ["Brightness", "80%"], ["Drone accent", "1"], ["Meta accent", "2"],
    ["Tracker accent", "3"], ["Flock accent", "4"],
    ["Wi-Fi attack accent", "5"], ["Clear accent", "6"],
  ]);
  assert.equal(themeDraft.snapshot().draft.brightness, 70);

  const currentPolicy = completePolicy();
  const policyDraft = badgeView.createDraftState(badgeView.validatePolicy);
  policyDraft.observe(currentPolicy);
  const editedPolicy = structuredClone(currentPolicy);
  editedPolicy.classes.drone.priority = 99;
  policyDraft.edit(editedPolicy);
  assert.deepEqual(badgeView.policySnapshotRows(policyDraft.snapshot().current), [
    ["drone", "Yes", "lower", "near", "0"],
    ["meta", "Yes", "lower", "near", "1"],
    ["tracker", "Yes", "lower", "near", "2"],
    ["wifi_attack", "Yes", "lower", "near", "3"],
    ["skimmer", "Yes", "lower", "near", "4"],
    ["camera", "Yes", "lower", "near", "5"],
    ["flock", "Yes", "lower", "near", "6"],
    ["lock", "Yes", "lower", "near", "7"],
    ["hid", "Yes", "lower", "near", "8"],
    ["beacon", "Yes", "lower", "near", "9"],
    ["event_badge", "Yes", "lower", "near", "10"],
    ["auracast", "Yes", "lower", "near", "11"],
    ["scanner_status", "Yes", "lower", "near", "12"],
  ]);
  assert.equal(policyDraft.snapshot().draft.classes.drone.priority, 99);
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
