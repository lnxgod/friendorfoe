import { getHistory, getState } from "./api.js";
import { stableEntityKey, writePreference, readPreference } from "./ui.js";
import { renderLive, visibleEntities } from "./views/live.js";
import {
  createMap,
  invalidateSize,
  renderActiveEntities,
  renderTrail,
} from "./views/map.js";

const POLL_INTERVAL_MS = 1000;
const MAX_TRAIL_PAGES = 4;
const MAX_TRAIL_ROWS = 2000;
const VIEW_KEY = "newDash.v1.selectedView";
const FILTER_KEY = "newDash.v1.presentationFilters";
const VIEWS = ["live", "map", "history", "badge"];
const REMOTE_ID_SOURCES = new Set(["ble_rid", "wifi_rid"]);
const FILTER_VALUES = {
  class: new Set(["all", "drone", "meta", "tracker", "wifi_attack", "other"]),
  source: new Set([
    "all",
    "ble_rid",
    "wifi_rid",
    "wifi_dji_ie",
    "wifi_ssid",
    "wifi_oui",
    "wifi_probe",
    "ble_fingerprint",
    "wifi_assoc",
    "wifi_inventory",
  ]),
  minConfidence: new Set(["all", "50", "70", "90"]),
  freshness: new Set(["all", "live", "stale"]),
};
const DEFAULT_FILTERS = {
  class: "all",
  source: "all",
  minConfidence: "all",
  freshness: "all",
};

const tabs = [...document.querySelectorAll("[data-view-target]")];
const sections = new Map(VIEWS.map((view) => [view, document.querySelector(`#view-${view}`)]));
const filterPanel = document.querySelector("#presentation-filter-panel");
const filterForm = document.querySelector("#presentation-filters");
const filterSummary = document.querySelector("#filter-summary");
const desktopFilterMedia = window.matchMedia("(min-width: 760px)");
const requestStatus = document.querySelector("#request-status");
const liveRoot = document.querySelector("#live-root");
const mapCanvas = document.querySelector("#map-canvas");
const filterControls = {
  class: document.querySelector("#filter-class"),
  source: document.querySelector("#filter-source"),
  minConfidence: document.querySelector("#filter-confidence"),
  freshness: document.querySelector("#filter-freshness"),
};

let selectedView = readPreference(VIEW_KEY, (value) => VIEWS.includes(value), "live");
let filters = readPreference(FILTER_KEY, validFilters, DEFAULT_FILTERS);
let latestState = null;
let pollTimer = null;
let mapCreated = false;
let trailFetchStarted = false;

function validFilters(value) {
  return value
    && typeof value === "object"
    && FILTER_VALUES.class.has(value.class)
    && FILTER_VALUES.source.has(value.source)
    && FILTER_VALUES.minConfidence.has(value.minConfidence)
    && FILTER_VALUES.freshness.has(value.freshness);
}

function syncFilterControls() {
  for (const [name, control] of Object.entries(filterControls)) {
    control.value = filters[name];
  }
  const active = [];
  if (filters.class !== "all") active.push(`Class: ${filters.class}`);
  if (filters.source !== "all") active.push(`Source: ${filters.source}`);
  if (filters.minConfidence !== "all") active.push(`Confidence: ${filters.minConfidence}%+`);
  if (filters.freshness !== "all") active.push(filters.freshness === "live" ? "Live" : "Host-stale");
  filterSummary.textContent = active.length ? active.join(" · ") : "All";
}

function syncFilterDisclosure(event) {
  if (event.matches) {
    filterPanel.open = true;
  } else if (!event.initial) {
    filterPanel.open = false;
  }
}

function showRequestStatus(message = "") {
  requestStatus.hidden = !message;
  requestStatus.textContent = message;
}

function phaseLabel(connection) {
  const labels = {
    live: "USB connected",
    stale: "USB connected; status stale",
    waiting: "Waiting for badge",
    connecting: "Connecting",
    verifying: "Verifying badge",
    reconnecting: "Reconnecting",
    error: "USB error",
    unavailable: "USB status unavailable",
  };
  return labels[connection?.phase] || connection?.phase || "USB status unavailable";
}

function renderHeader(state) {
  const connection = state?.connection || {};
  const freshness = state?.freshness || {};
  const status = state?.status || {};
  const phase = document.querySelector("#connection-phase");
  phase.textContent = phaseLabel(connection);
  phase.classList.toggle("is-live", connection.phase === "live" && freshness.state === "fresh");
  phase.classList.toggle("is-warning", connection.phase !== "live" || freshness.state === "stale");
  document.querySelector("#connection-freshness").textContent = freshness.age_s === null || freshness.age_s === undefined
    ? "Freshness unavailable"
    : `${freshness.state === "fresh" ? "Fresh" : "Stale"} ${freshness.age_s} s`;
  document.querySelector("#connection-firmware").textContent = status.version || connection.firmware_version
    ? `FW ${status.version || connection.firmware_version}`
    : "Firmware missing";
  const scanners = Array.isArray(status.scanners) ? status.scanners : [];
  const scannerSuffix = status.sensing_health ? ` · ${status.sensing_health.replaceAll("_", " ")}` : "";
  document.querySelector("#connection-scanners").textContent = scanners.length
    ? `${scanners.length} ${scanners.length === 1 ? "scanner" : "scanners"}${scannerSuffix}`
    : `Scanners missing${scannerSuffix}`;
  document.querySelector("#connection-port").textContent = connection.port || "Port missing";
}

function mapEntities(state) {
  return visibleEntities(state, filters).filter((entity) => REMOTE_ID_SOURCES.has(entity.source));
}

function renderState(state) {
  renderHeader(state);
  renderLive(liveRoot, state, filters);
  if (mapCreated) {
    renderActiveEntities(mapEntities(state));
  }
  if (selectedView === "map") {
    ensureMapTrails();
  }
}

function ensureMap() {
  if (!mapCreated) {
    createMap(mapCanvas);
    mapCreated = true;
  }
  window.requestAnimationFrame(() => invalidateSize());
  if (latestState) {
    renderActiveEntities(mapEntities(latestState));
    ensureMapTrails();
  }
}

async function ensureMapTrails() {
  if (trailFetchStarted || !latestState || selectedView !== "map") {
    return;
  }
  const keys = [...new Set(
    mapEntities(latestState).map((entity) => stableEntityKey(entity)),
  )];
  if (!keys.length) {
    return;
  }
  trailFetchStarted = true;
  const queue = keys.map((key) => ({ key, cursor: null }));
  const points = [];
  let pageCount = 0;
  try {
    while (queue.length && pageCount < MAX_TRAIL_PAGES && points.length < MAX_TRAIL_ROWS) {
      const job = queue.shift();
      const query = new URLSearchParams();
      query.set("kind", "track");
      query.set("positioned", "true");
      query.set("text", job.key);
      query.set("limit", "500");
      if (job.cursor) {
        query.set("cursor", job.cursor);
      }
      const page = await getHistory(query);
      pageCount += 1;
      const remaining = MAX_TRAIL_ROWS - points.length;
      const exactItems = (Array.isArray(page?.items) ? page.items : [])
        .filter((item) => item?.stable_key === job.key)
        .slice(0, remaining);
      points.push(...exactItems);
      if (page?.next_cursor && pageCount < MAX_TRAIL_PAGES && points.length < MAX_TRAIL_ROWS) {
        queue.push({ key: job.key, cursor: page.next_cursor });
      }
    }
    renderTrail(points);
  } catch (error) {
    showRequestStatus(`Host-observed trail unavailable: ${error.message}`);
  }
}

function activateView(view, { focus = true, replaceHash = false } = {}) {
  const validView = VIEWS.includes(view) ? view : "live";
  selectedView = validView;
  writePreference(VIEW_KEY, selectedView);
  for (const tab of tabs) {
    const selected = tab.dataset.viewTarget === selectedView;
    tab.setAttribute("aria-selected", selected ? "true" : "false");
    tab.tabIndex = selected ? 0 : -1;
  }
  for (const [name, section] of sections) {
    section.hidden = name !== selectedView;
  }
  filterPanel.hidden = !["live", "map"].includes(selectedView);
  const desiredHash = `#${selectedView}`;
  if (window.location.hash !== desiredHash) {
    if (replaceHash) {
      window.history.replaceState(null, "", desiredHash);
    } else {
      window.location.hash = selectedView;
    }
  }
  if (selectedView === "map") {
    ensureMap();
  }
  if (focus) {
    sections.get(selectedView).focus({ preventScroll: true });
  }
}

function routeFromHash({ focus = true } = {}) {
  const hashView = window.location.hash.replace(/^#/, "");
  if (!hashView) {
    activateView(selectedView, { focus, replaceHash: true });
    return;
  }
  activateView(VIEWS.includes(hashView) ? hashView : "live", {
    focus,
    replaceHash: !VIEWS.includes(hashView),
  });
}

for (const tab of tabs) {
  tab.addEventListener("click", () => activateView(tab.dataset.viewTarget));
  tab.addEventListener("keydown", (event) => {
    const current = tabs.indexOf(tab);
    let next = null;
    if (event.key === "ArrowLeft") {
      next = (current - 1 + tabs.length) % tabs.length;
    } else if (event.key === "ArrowRight") {
      next = (current + 1) % tabs.length;
    } else if (event.key === "Home") {
      next = 0;
    } else if (event.key === "End") {
      next = tabs.length - 1;
    }
    if (next !== null) {
      event.preventDefault();
      tabs[next].focus();
      activateView(tabs[next].dataset.viewTarget, { focus: false });
    }
  });
}

filterForm.addEventListener("change", () => {
  const next = {
    class: filterControls.class.value,
    source: filterControls.source.value,
    minConfidence: filterControls.minConfidence.value,
    freshness: filterControls.freshness.value,
  };
  filters = validFilters(next) ? next : DEFAULT_FILTERS;
  syncFilterControls();
  writePreference(FILTER_KEY, filters);
  if (latestState) {
    renderState(latestState);
  }
});

filterForm.addEventListener("reset", () => {
  window.setTimeout(() => {
    filters = { ...DEFAULT_FILTERS };
    syncFilterControls();
    writePreference(FILTER_KEY, filters);
    if (latestState) {
      renderState(latestState);
    }
  }, 0);
});

window.addEventListener("hashchange", () => routeFromHash({ focus: true }));
desktopFilterMedia.addEventListener("change", syncFilterDisclosure);

async function pollState() {
  try {
    const state = await getState();
    latestState = state;
    showRequestStatus();
    renderState(state);
  } catch (error) {
    showRequestStatus(`${error.message} Last valid dashboard state is retained.`);
  } finally {
    pollTimer = window.setTimeout(pollState, POLL_INTERVAL_MS);
  }
}

window.addEventListener("pagehide", () => {
  if (pollTimer !== null) {
    window.clearTimeout(pollTimer);
  }
});

syncFilterControls();
syncFilterDisclosure({ matches: desktopFilterMedia.matches, initial: desktopFilterMedia.matches });
routeFromHash({ focus: false });
pollState();
