import {
  element,
  formatAge,
  formatCoordinates,
  replace,
  stableEntityKey,
  validCoordinatePair,
} from "../ui.js";

const REMOTE_ID_SOURCES = new Set(["ble_rid", "wifi_rid"]);
const OFFLINE_MESSAGE = "Basemap offline — coordinates and observations remain available";
const TRAIL_LABEL = "Host-observed trail";
const WORLD_VIEW = [20, 0];
const HISTORY_TRAIL_PAGE_SIZE = 500;
const DISTINCT_TRAIL_PAGE_SIZE = 4096;
const MAX_HISTORY_TRAIL_PAGES = 4;
const MAX_HISTORY_TRAIL_ROWS = 2000;
const MAX_DISTINCT_TRAIL_PAGES = 2;
const MAX_DISTINCT_TRAIL_ROWS = 8192;
const MAX_TRAIL_ATTEMPTS = 2;
const ALL_POSITIONED_TRAILS = "all-positioned-remote-id";
const OPERATOR_LINK_COLOR = "#70838c";
const OPERATOR_LINK_MAX_OPACITY = 0.16;
export const MIN_TRAIL_RETENTION_MINUTES = 1;
export const MAX_TRAIL_RETENTION_MINUTES = 120;
export const DEFAULT_TRAIL_RETENTION_MINUTES = 120;

export function createRequestStatusChannels({ render }) {
  let stateMessage = "";
  let trailMessage = "";

  function publish() {
    render([stateMessage, trailMessage].filter(Boolean).join(" "));
  }

  return {
    setState(message) {
      stateMessage = message;
      publish();
    },
    clearState() {
      stateMessage = "";
      publish();
    },
    setTrail(message) {
      trailMessage = message;
      publish();
    },
    clearTrail() {
      trailMessage = "";
      publish();
    },
  };
}

let map = null;
let tileLayer = null;
let entityLayer = null;
let trailLayer = null;
let offlineNotice = null;
let semanticList = null;
let selectedLabel = null;
let trailDotCount = null;
let markerByKey = new Map();
let visualsByKey = new Map();
let activeByKey = new Map();
let activeFingerprint = "";
let selectedKey = null;
let onSelectionChange = () => {};
let trailRetentionSeconds = DEFAULT_TRAIL_RETENTION_MINUTES * 60;
let lastTrailPoints = [];
let lastTrailTruncated = false;
let hasFittedActiveBounds = false;
let tileErrors = 0;
let tileSucceeded = false;

function markerIcon(kind) {
  return window.L.divIcon({
    className: `instrument-marker ${kind}-marker`,
    iconSize: [26, 26],
    iconAnchor: [13, 13],
  });
}

function showOfflineNotice(show) {
  if (!offlineNotice) {
    return;
  }
  offlineNotice.hidden = !show;
  offlineNotice.textContent = show ? OFFLINE_MESSAGE : "";
}

function handleTileError() {
  if (tileSucceeded) {
    return;
  }
  tileErrors += 1;
  if (tileErrors >= 3) {
    showOfflineNotice(true);
  }
}

function handleTileSuccess() {
  tileSucceeded = true;
  tileErrors = 0;
  showOfflineNotice(false);
}

export function createMap(element, options = {}) {
  onSelectionChange = typeof options.onSelectionChange === "function"
    ? options.onSelectionChange
    : () => {};
  if (typeof options.selectedKey === "string" && options.selectedKey) {
    selectedKey = options.selectedKey;
  }
  if (map) {
    return map;
  }
  if (!window.L) {
    throw new Error("Leaflet is unavailable.");
  }
  offlineNotice = document.querySelector("#map-offline");
  semanticList = document.querySelector("#map-entity-list");
  selectedLabel = document.querySelector("#map-selected");
  trailDotCount = document.querySelector("#map-trail-dot-count");
  map = window.L.map(element, { zoomControl: true, preferCanvas: true });
  tileLayer = window.L.tileLayer(
    "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
    {
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a>',
      maxZoom: 19,
    },
  );
  tileLayer.on("tileerror", handleTileError);
  tileLayer.on("tileload", handleTileSuccess);
  tileLayer.addTo(map);
  window.L.control.scale({ imperial: true, metric: true }).addTo(map);
  entityLayer = window.L.layerGroup().addTo(map);
  trailLayer = window.L.layerGroup().addTo(map);
  map.setView(WORLD_VIEW, 2);
  return map;
}

function positionedRemoteId(entities) {
  return entities.filter((entity) => (
    entity
    && REMOTE_ID_SOURCES.has(entity.source)
    && entity.stale !== true
    && validCoordinatePair(entity.lat, entity.lon)
  ));
}

export function defaultSelectedEntityKey(entities) {
  let bestKey = null;
  let bestWeight = Number.NEGATIVE_INFINITY;
  for (const entity of Array.isArray(entities) ? entities : []) {
    const weight = Math.max(
      Number.isFinite(Number(entity?.seen_count)) ? Number(entity.seen_count) : 0,
      Number.isFinite(Number(entity?.events)) ? Number(entity.events) : 0,
    );
    if (bestKey === null || weight > bestWeight) {
      bestKey = stableEntityKey(entity);
      bestWeight = weight;
    }
  }
  return bestKey;
}

export function retainedSelectedEntityKey(entities, currentKey) {
  const positioned = Array.isArray(entities) ? entities : [];
  if (currentKey && positioned.some((entity) => stableEntityKey(entity) === currentKey)) {
    return currentKey;
  }
  return defaultSelectedEntityKey(positioned);
}

export function positionOpacity(entity) {
  const age = Number(entity?.host_age_s);
  const retention = Number(entity?.position_retention_s);
  if (!Number.isFinite(age) || !Number.isFinite(retention) || retention <= 0) {
    return 1;
  }
  const fraction = Math.min(Math.max(age / retention, 0), 1);
  return Math.max(0.2, 1 - fraction * 0.8);
}

function markerButton(entity, key) {
  const button = element("button", {
    attributes: { type: "button", "data-stable-key": key },
  });
  button.addEventListener("click", () => focusEntity(key));
  return button;
}

function updateEntityAppearance(entity, key) {
  const visual = visualsByKey.get(key);
  if (!visual) return;
  const opacity = positionOpacity(entity);
  const age = Number(entity.host_age_s);
  const ageLabel = Number.isFinite(age) ? ` · GPS ${formatAge(age)}` : "";
  const identity = entity.display_id || entity.label || "Identity missing";
  visual.button.textContent = `${identity} · ${formatCoordinates(entity.lat, entity.lon)}${ageLabel}`;
  visual.button.style.opacity = String(opacity);
  visual.drone.setOpacity(opacity);
  visual.operator?.setOpacity(opacity);
  visual.link?.setStyle({ opacity: OPERATOR_LINK_MAX_OPACITY * opacity });
}

function updateSelectionAppearance() {
  for (const [key, marker] of markerByKey) {
    marker.getElement()?.classList.toggle("selected-marker", key === selectedKey);
  }
  if (semanticList) {
    for (const button of semanticList.querySelectorAll("button[data-stable-key]")) {
      button.setAttribute("aria-current", button.dataset.stableKey === selectedKey ? "true" : "false");
    }
  }
  const entity = activeByKey.get(selectedKey);
  if (selectedLabel) {
    selectedLabel.textContent = entity?.display_id || entity?.label || "None";
  }
}

export function renderActiveEntities(entities) {
  if (!map || !entityLayer) {
    return;
  }
  const positioned = positionedRemoteId(Array.isArray(entities) ? entities : []);
  const fingerprint = JSON.stringify(positioned.map((entity) => [
    stableEntityKey(entity),
    entity.lat,
    entity.lon,
    entity.operator_lat,
    entity.operator_lon,
  ]));
  if (fingerprint === activeFingerprint) {
    for (const entity of positioned) {
      const key = stableEntityKey(entity);
      activeByKey.set(key, entity);
      updateEntityAppearance(entity, key);
    }
    updateSelectionAppearance();
    return;
  }
  activeFingerprint = fingerprint;
  entityLayer.clearLayers();
  markerByKey = new Map();
  visualsByKey = new Map();
  activeByKey = new Map();
  const listItems = [];
  const bounds = [];
  for (const entity of positioned) {
    const key = stableEntityKey(entity);
    activeByKey.set(key, entity);
    const dronePosition = [entity.lat, entity.lon];
    const drone = window.L.marker(dronePosition, {
      icon: markerIcon("drone"),
      keyboard: false,
      alt: "Remote ID drone marker",
    }).addTo(entityLayer);
    drone.on("click", () => focusEntity(key));
    markerByKey.set(key, drone);
    bounds.push(dronePosition);
    const button = markerButton(entity, key);
    listItems.push(button);
    let operator = null;
    let link = null;

    if (validCoordinatePair(entity.operator_lat, entity.operator_lon)) {
      const operatorPosition = [entity.operator_lat, entity.operator_lon];
      operator = window.L.marker(operatorPosition, {
        icon: markerIcon("operator"),
        keyboard: false,
        alt: "Remote ID operator marker",
      }).addTo(entityLayer);
      link = window.L.polyline([dronePosition, operatorPosition], {
        color: OPERATOR_LINK_COLOR,
        weight: 1,
        opacity: OPERATOR_LINK_MAX_OPACITY,
      }).addTo(entityLayer);
      bounds.push(operatorPosition);
    }
    visualsByKey.set(key, { drone, operator, link, button });
    updateEntityAppearance(entity, key);
  }
  if (semanticList) {
    replace(
      semanticList,
      listItems.length ? listItems : [element("p", { className: "empty-state", text: "No positioned Remote ID entities." })],
    );
  }
  const nextSelectedKey = retainedSelectedEntityKey(positioned, selectedKey);
  if (nextSelectedKey !== selectedKey) {
    selectedKey = nextSelectedKey;
  }
  updateSelectionAppearance();
  if (bounds.length && !hasFittedActiveBounds) {
    map.fitBounds(bounds, { padding: [48, 48], maxZoom: 15 });
    hasFittedActiveBounds = true;
  }
}

export function uniqueVisibleTrailPoints(points, cutoff) {
  const visible = [];
  const coordinates = new Set();
  for (const point of Array.isArray(points) ? points : []) {
    if (!point || !validCoordinatePair(point.latitude, point.longitude)) {
      continue;
    }
    const observedAt = Number(point.observed_at);
    if (Number.isFinite(observedAt) && observedAt < cutoff) {
      continue;
    }
    const coordinateKey = `${Number(point.latitude).toFixed(7)},${Number(point.longitude).toFixed(7)}`;
    if (coordinates.has(coordinateKey)) {
      continue;
    }
    coordinates.add(coordinateKey);
    visible.push(point);
  }
  return visible;
}

export function trailDotCountLabel(count, truncated = false) {
  const numeric = Number(count);
  const normalized = Number.isFinite(numeric) ? Math.max(Math.trunc(numeric), 0) : 0;
  const suffix = truncated ? "+" : "";
  const unit = normalized === 1 && !truncated ? "dot" : "dots";
  return `${normalized.toLocaleString()}${suffix} ${unit}`;
}

export function renderTrail(points, { fit = true, truncated = false } = {}) {
  lastTrailPoints = Array.isArray(points) ? points : [];
  lastTrailTruncated = truncated === true;
  if (!map || !trailLayer) {
    return;
  }
  trailLayer.clearLayers();
  const cutoff = Date.now() / 1000 - trailRetentionSeconds;
  const visiblePoints = uniqueVisibleTrailPoints(lastTrailPoints, cutoff);
  const trailPositions = [];
  for (const point of visiblePoints) {
    const position = [point.latitude, point.longitude];
    window.L.circleMarker(position, {
      radius: 3,
      color: "#54d8ec",
      fillColor: "#54d8ec",
      fillOpacity: 0.82,
      opacity: 0.95,
      weight: 1,
      interactive: false,
    }).addTo(trailLayer);
    trailPositions.push(position);
  }
  if (trailDotCount) {
    trailDotCount.textContent = trailDotCountLabel(visiblePoints.length, lastTrailTruncated);
    trailDotCount.title = lastTrailTruncated
      ? "More retained positions are available than this map drawing budget can show."
      : "";
  }
  if (fit && trailPositions.length) {
    map.fitBounds(trailPositions, { padding: [48, 48], maxZoom: 17 });
  }
  void TRAIL_LABEL;
}

export function setTrailRetentionSeconds(seconds) {
  const numeric = Number(seconds);
  const minimum = MIN_TRAIL_RETENTION_MINUTES * 60;
  const maximum = MAX_TRAIL_RETENTION_MINUTES * 60;
  trailRetentionSeconds = Number.isFinite(numeric)
    ? Math.min(Math.max(numeric, minimum), maximum)
    : DEFAULT_TRAIL_RETENTION_MINUTES * 60;
  renderTrail(lastTrailPoints, { fit: false, truncated: lastTrailTruncated });
  return trailRetentionSeconds;
}

export function createTrailController({
  getHistory,
  render,
  reportError,
  clearError = () => {},
  retentionSeconds = () => DEFAULT_TRAIL_RETENTION_MINUTES * 60,
  minimumSinceSeconds = () => null,
  nowSeconds = () => Date.now() / 1000,
  allPositioned = false,
}) {
  let generation = 0;
  let signature = "";
  let currentKeys = [];
  let state = "idle";
  let attempts = 0;
  let activeController = null;
  let activePromise = null;
  let fitOnLoad = true;

  async function load(loadGeneration, controller, fit) {
    const acceptedKeys = allPositioned ? null : new Set(currentKeys);
    const queue = allPositioned
      ? [{ key: null, cursor: null }]
      : currentKeys.map((key) => ({ key, cursor: null }));
    const points = [];
    const requestedRetention = Number(retentionSeconds());
    const requestedNow = Number(nowSeconds());
    const retentionCutoff = Number.isFinite(requestedRetention) && Number.isFinite(requestedNow)
      ? Math.floor(requestedNow - requestedRetention)
      : null;
    const requestedMinimum = minimumSinceSeconds();
    const minimumCutoff = requestedMinimum === null || requestedMinimum === undefined
      ? null
      : Number(requestedMinimum);
    const cutoff = Number.isFinite(minimumCutoff)
      ? Math.max(minimumCutoff, retentionCutoff ?? minimumCutoff)
      : retentionCutoff;
    const pageSize = allPositioned ? DISTINCT_TRAIL_PAGE_SIZE : HISTORY_TRAIL_PAGE_SIZE;
    const maximumPages = allPositioned ? MAX_DISTINCT_TRAIL_PAGES : MAX_HISTORY_TRAIL_PAGES;
    const maximumRows = allPositioned ? MAX_DISTINCT_TRAIL_ROWS : MAX_HISTORY_TRAIL_ROWS;
    let pageCount = 0;
    let truncated = false;
    try {
      while (queue.length && pageCount < maximumPages && points.length < maximumRows) {
        const job = queue.shift();
        const query = new URLSearchParams();
        query.set("kind", "track");
        query.set("positioned", "true");
        if (allPositioned) {
          query.set("distinct_coordinates", "true");
        }
        if (job.key) {
          query.set("text", job.key);
        }
        query.set("limit", String(pageSize));
        if (cutoff !== null) {
          query.set("since", String(cutoff));
        }
        if (job.cursor) {
          query.set("cursor", job.cursor);
        }
        const page = await getHistory(query, { signal: controller.signal });
        if (loadGeneration !== generation || controller.signal.aborted) {
          return;
        }
        pageCount += 1;
        const remaining = maximumRows - points.length;
        const eligibleItems = (Array.isArray(page?.items) ? page.items : [])
          .filter((item) => {
            if (acceptedKeys && !acceptedKeys.has(item?.stable_key)) {
              return false;
            }
            const observedAt = Number(item?.observed_at);
            return cutoff === null || !Number.isFinite(observedAt) || observedAt >= cutoff;
          });
        const exactItems = eligibleItems.slice(0, remaining);
        if (eligibleItems.length > remaining) {
          truncated = true;
        }
        points.push(...exactItems);
        if (page?.next_cursor) {
          if (pageCount < maximumPages && points.length < maximumRows) {
            queue.push({ key: job.key, cursor: page.next_cursor });
          } else {
            truncated = true;
          }
        }
      }
      if (queue.length) {
        truncated = true;
      }
      if (loadGeneration === generation && !controller.signal.aborted) {
        state = "loaded";
        clearError();
        render(points, { fit, truncated });
      }
    } catch (error) {
      if (loadGeneration === generation && !controller.signal.aborted) {
        state = "failed";
        reportError(error);
      }
    }
  }

  function startLoad() {
    attempts += 1;
    state = "loading";
    const loadGeneration = generation;
    const controller = new AbortController();
    activeController = controller;
    const promise = load(loadGeneration, controller, fitOnLoad).finally(() => {
      if (activePromise === promise) {
        activePromise = null;
        activeController = null;
      }
    });
    activePromise = promise;
    return promise;
  }

  function updateKeys(keys, { forceReset = false, preserve = false, fit = !preserve } = {}) {
    const nextKeys = allPositioned
      ? [ALL_POSITIONED_TRAILS]
      : [...new Set(
        (Array.isArray(keys) ? keys : []).filter((key) => typeof key === "string" && key),
      )].sort();
    const nextSignature = JSON.stringify(nextKeys);
    if (forceReset || nextSignature !== signature) {
      generation += 1;
      activeController?.abort();
      activeController = null;
      activePromise = null;
      signature = nextSignature;
      currentKeys = nextKeys;
      attempts = 0;
      state = "idle";
      fitOnLoad = fit;
      if (!preserve || !currentKeys.length) {
        clearError();
      }
      if (!preserve || !currentKeys.length) {
        render([]);
      }
    }
    if (!currentKeys.length || state === "loaded" || attempts >= MAX_TRAIL_ATTEMPTS) {
      return Promise.resolve();
    }
    if (activePromise) {
      return activePromise;
    }
    return startLoad();
  }

  return {
    update(keys, { preserve = false, fit = !preserve } = {}) {
      return updateKeys(keys, { preserve, fit });
    },
    refresh(keys, { preserve = false, fit = !preserve } = {}) {
      return updateKeys(keys, { forceReset: true, preserve, fit });
    },
  };
}

export function focusEntity(stableKey) {
  if (!map || !activeByKey.has(stableKey)) {
    return false;
  }
  const changed = selectedKey !== stableKey;
  selectedKey = stableKey;
  updateSelectionAppearance();
  const entity = activeByKey.get(stableKey);
  map.panTo([entity.lat, entity.lon]);
  if (changed) {
    onSelectionChange(selectedKey);
  }
  return true;
}

export function invalidateSize() {
  map?.invalidateSize();
}

export function destroy() {
  if (!map) {
    return;
  }
  tileLayer?.off("tileerror", handleTileError);
  tileLayer?.off("tileload", handleTileSuccess);
  map.remove();
  map = null;
  tileLayer = null;
  entityLayer = null;
  trailLayer = null;
  trailDotCount = null;
  markerByKey = new Map();
  activeByKey = new Map();
  activeFingerprint = "";
  selectedKey = null;
  onSelectionChange = () => {};
  lastTrailPoints = [];
  lastTrailTruncated = false;
  trailRetentionSeconds = DEFAULT_TRAIL_RETENTION_MINUTES * 60;
  hasFittedActiveBounds = false;
  tileErrors = 0;
  tileSucceeded = false;
}
