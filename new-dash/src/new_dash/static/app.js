import { createCompletionPoller, getHistory, getState, post } from "./api.js";
import { nextTabIndex, scannerSummary, writePreference, readPreference } from "./ui.js";
import {
  renderLive,
  visiblePositionedRemoteIds,
} from "./views/live.js";
import {
  createMap,
  createRequestStatusChannels,
  createTrailController,
  DEFAULT_TRAIL_RETENTION_MINUTES,
  invalidateSize,
  MAX_TRAIL_RETENTION_MINUTES,
  MIN_TRAIL_RETENTION_MINUTES,
  renderActiveEntities,
  renderTrail,
  setTrailRetentionSeconds,
} from "./views/map.js";
import { createHistoryView } from "./views/history.js";
import { createBadgeView } from "./views/badge.js";

const POLL_INTERVAL_MS = 1000;
const TRAIL_REFRESH_INTERVAL_MS = 5000;
const VIEW_KEY = "newDash.v1.selectedView";
const FILTER_KEY = "newDash.v1.presentationFilters";
const TRAIL_RETENTION_KEY = "newDash.v2.mapFormationMinutes";
const TRAIL_SELECTION_KEY = "newDash.v1.mapSelectedRemoteId";
const VIEWS = ["live", "map", "history", "badge"];
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
const trailMinutesControl = document.querySelector("#map-trail-minutes");
const trailMinutesOutput = document.querySelector("#map-trail-minutes-output");
const drawingResetControl = document.querySelector("#map-drawing-reset");
const drawingElapsedOutput = document.querySelector("#map-drawing-elapsed");
const filterControls = {
  class: document.querySelector("#filter-class"),
  source: document.querySelector("#filter-source"),
  minConfidence: document.querySelector("#filter-confidence"),
  freshness: document.querySelector("#filter-freshness"),
};

let selectedView = readPreference(VIEW_KEY, (value) => VIEWS.includes(value), "live");
let filters = readPreference(FILTER_KEY, validFilters, DEFAULT_FILTERS);
let trailRetentionMinutes = readPreference(
  TRAIL_RETENTION_KEY,
  validTrailRetentionMinutes,
  DEFAULT_TRAIL_RETENTION_MINUTES,
);
let trailSelectedKey = readPreference(
  TRAIL_SELECTION_KEY,
  (value) => typeof value === "string" && Boolean(value),
  null,
);
let latestState = null;
let mapCreated = false;
let activeView = null;
let trailRefreshTimer = null;
let trailRefreshInFlight = false;
let drawingStartedAt = null;
let initialTrailFitPending = true;

const historyView = createHistoryView({ getHistory, post });
const badgeView = createBadgeView({ post });

function validFilters(value) {
  return value
    && typeof value === "object"
    && FILTER_VALUES.class.has(value.class)
    && FILTER_VALUES.source.has(value.source)
    && FILTER_VALUES.minConfidence.has(value.minConfidence)
    && FILTER_VALUES.freshness.has(value.freshness);
}

function validTrailRetentionMinutes(value) {
  return Number.isInteger(value)
    && value >= MIN_TRAIL_RETENTION_MINUTES
    && value <= MAX_TRAIL_RETENTION_MINUTES;
}

function syncTrailControl() {
  trailMinutesControl.value = String(trailRetentionMinutes);
  trailMinutesOutput.value = `${trailRetentionMinutes} min`;
  trailMinutesOutput.textContent = `${trailRetentionMinutes} min`;
  setTrailRetentionSeconds(trailRetentionMinutes * 60);
}

function formatElapsed(seconds) {
  const totalSeconds = Math.max(0, Math.floor(Number(seconds) || 0));
  const minutes = Math.floor(totalSeconds / 60);
  const remainder = String(totalSeconds % 60).padStart(2, "0");
  return `${minutes}:${remainder}`;
}

function syncDrawingSession() {
  if (drawingStartedAt === null) {
    drawingElapsedOutput.value = "Saved trail shown";
    drawingElapsedOutput.textContent = "Saved trail shown";
    drawingResetControl.textContent = "Clear & start timer";
    return;
  }
  const elapsed = `${formatElapsed(Date.now() / 1000 - drawingStartedAt)} elapsed`;
  drawingElapsedOutput.value = elapsed;
  drawingElapsedOutput.textContent = elapsed;
  drawingResetControl.textContent = "Clear & restart timer";
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

const requestStatuses = createRequestStatusChannels({ render: showRequestStatus });

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
  document.querySelector("#connection-scanners").textContent = scannerSummary(status);
  document.querySelector("#connection-port").textContent = connection.port || "Port missing";
}

function mapEntities(state) {
  return visiblePositionedRemoteIds(state, filters, drawingStartedAt);
}

function renderState(state) {
  renderHeader(state);
  syncDrawingSession();
  renderLive(liveRoot, state, filters);
  historyView.observeState(state);
  badgeView.render(state);
  if (mapCreated) {
    renderActiveEntities(mapEntities(state));
  }
  if (selectedView === "map") {
    ensureMapTrails();
  }
}

function ensureMap(refreshTrails = false) {
  if (!mapCreated) {
    createMap(mapCanvas, {
      selectedKey: trailSelectedKey,
      onSelectionChange: (key) => {
        trailSelectedKey = key;
        writePreference(TRAIL_SELECTION_KEY, trailSelectedKey);
      },
    });
    mapCreated = true;
  }
  window.requestAnimationFrame(() => invalidateSize());
  if (latestState) {
    renderActiveEntities(mapEntities(latestState));
    ensureMapTrails(refreshTrails, {
      preserve: refreshTrails,
      fit: initialTrailFitPending,
    });
  }
}

async function ensureMapTrails(
  refresh = false,
  { preserve = false, fit = initialTrailFitPending } = {},
) {
  if (!latestState || selectedView !== "map") {
    return;
  }
  await (refresh
    ? trailController.refresh([], { preserve, fit })
    : trailController.update([], { preserve, fit }));
}

async function refreshFormationTrail() {
  if (trailRefreshInFlight || selectedView !== "map") {
    return;
  }
  trailRefreshInFlight = true;
  try {
    await ensureMapTrails(true, { preserve: true, fit: initialTrailFitPending });
  } finally {
    trailRefreshInFlight = false;
  }
}

function syncTrailRefreshTimer() {
  if (selectedView !== "map") {
    if (trailRefreshTimer !== null) {
      window.clearTimeout(trailRefreshTimer);
      trailRefreshTimer = null;
    }
    return;
  }
  if (trailRefreshTimer === null) {
    trailRefreshTimer = window.setTimeout(async () => {
      trailRefreshTimer = null;
      await refreshFormationTrail();
      syncTrailRefreshTimer();
    }, TRAIL_REFRESH_INTERVAL_MS);
  }
}

function activateView(view, { focus = true, replaceHash = false, keyboard = false } = {}) {
  const validView = VIEWS.includes(view) ? view : "live";
  const previousView = activeView;
  selectedView = validView;
  activeView = selectedView;
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
    } else if (keyboard) {
      window.history.pushState(null, "", desiredHash);
    } else {
      window.location.hash = selectedView;
    }
  }
  if (selectedView === "map") {
    ensureMap(previousView !== "map");
  }
  syncTrailRefreshTimer();
  if (previousView === "history" && selectedView !== "history") {
    historyView.deactivate();
  }
  if (selectedView === "history") {
    void historyView.activate(latestState);
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
    const next = nextTabIndex(current, event.key, tabs.length);
    if (next !== null) {
      event.preventDefault();
      tabs[next].focus();
      activateView(tabs[next].dataset.viewTarget, { focus: false, keyboard: true });
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
window.addEventListener("popstate", () => routeFromHash({ focus: true }));
desktopFilterMedia.addEventListener("change", syncFilterDisclosure);

trailMinutesControl.addEventListener("input", () => {
  const next = Number.parseInt(trailMinutesControl.value, 10);
  trailRetentionMinutes = validTrailRetentionMinutes(next)
    ? next
    : DEFAULT_TRAIL_RETENTION_MINUTES;
  syncTrailControl();
  writePreference(TRAIL_RETENTION_KEY, trailRetentionMinutes);
});

trailMinutesControl.addEventListener("change", () => {
  void ensureMapTrails(true, { preserve: true, fit: false });
});

drawingResetControl.addEventListener("click", () => {
  drawingStartedAt = Date.now() / 1000;
  syncDrawingSession();
  if (latestState) {
    renderActiveEntities(mapEntities(latestState));
  }
  void trailController.refresh([], { preserve: false, fit: false });
});

const trailController = createTrailController({
  getHistory,
  render: (points, options = {}) => {
    renderTrail(points, options);
    if (options.fit && points.length) {
      initialTrailFitPending = false;
    }
  },
  reportError: (error) => requestStatuses.setTrail(
    `Host-observed trail unavailable: ${error.message}`,
  ),
  clearError: () => requestStatuses.clearTrail(),
  retentionSeconds: () => trailRetentionMinutes * 60,
  minimumSinceSeconds: () => drawingStartedAt,
  allPositioned: true,
});

const statePoller = createCompletionPoller({
  load: (signal) => getState({ signal }),
  onValue: (state) => {
    latestState = state;
    requestStatuses.clearState();
    renderState(state);
  },
  onError: (error) => requestStatuses.setState(
    `${error.message} Last valid dashboard state is retained.`,
  ),
  intervalMs: POLL_INTERVAL_MS,
});

window.addEventListener("pagehide", () => {
  statePoller.stop();
  if (trailRefreshTimer !== null) {
    window.clearTimeout(trailRefreshTimer);
    trailRefreshTimer = null;
  }
});
window.addEventListener("pageshow", () => {
  statePoller.start();
  syncTrailRefreshTimer();
});

syncFilterControls();
syncTrailControl();
syncDrawingSession();
syncFilterDisclosure({ matches: desktopFilterMedia.matches, initial: desktopFilterMedia.matches });
routeFromHash({ focus: false });
statePoller.start();
