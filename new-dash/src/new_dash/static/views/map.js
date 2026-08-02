import {
  element,
  formatCoordinates,
  replace,
  stableEntityKey,
  validCoordinatePair,
} from "../ui.js";

const REMOTE_ID_SOURCES = new Set(["ble_rid", "wifi_rid"]);
const OFFLINE_MESSAGE = "Basemap offline — coordinates and observations remain available";
const TRAIL_LABEL = "Host-observed trail";
const WORLD_VIEW = [20, 0];

let map = null;
let tileLayer = null;
let entityLayer = null;
let trailLayer = null;
let offlineNotice = null;
let semanticList = null;
let selectedLabel = null;
let markerByKey = new Map();
let activeByKey = new Map();
let activeFingerprint = "";
let selectedKey = null;
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

export function createMap(element) {
  if (map) {
    return map;
  }
  if (!window.L) {
    throw new Error("Leaflet is unavailable.");
  }
  offlineNotice = document.querySelector("#map-offline");
  semanticList = document.querySelector("#map-entity-list");
  selectedLabel = document.querySelector("#map-selected");
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

function markerButton(entity, key) {
  const identity = entity.display_id || entity.label || "Identity missing";
  const button = element("button", {
    text: `${identity} · ${formatCoordinates(entity.lat, entity.lon)}`,
    attributes: { type: "button", "data-stable-key": key },
  });
  button.addEventListener("click", () => focusEntity(key));
  return button;
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
    return;
  }
  activeFingerprint = fingerprint;
  entityLayer.clearLayers();
  markerByKey = new Map();
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
    listItems.push(markerButton(entity, key));

    if (validCoordinatePair(entity.operator_lat, entity.operator_lon)) {
      const operatorPosition = [entity.operator_lat, entity.operator_lon];
      window.L.marker(operatorPosition, {
        icon: markerIcon("operator"),
        keyboard: false,
        alt: "Remote ID operator marker",
      }).addTo(entityLayer);
      window.L.polyline([dronePosition, operatorPosition], {
        color: "#54d8ec",
        weight: 2,
        opacity: 0.9,
      }).addTo(entityLayer);
      bounds.push(operatorPosition);
    }
  }
  if (semanticList) {
    replace(
      semanticList,
      listItems.length ? listItems : [element("p", { className: "empty-state", text: "No positioned Remote ID entities." })],
    );
  }
  if (!activeByKey.has(selectedKey)) {
    selectedKey = activeByKey.keys().next().value || null;
  }
  updateSelectionAppearance();
  if (bounds.length) {
    map.fitBounds(bounds, { padding: [48, 48], maxZoom: 15 });
  } else {
    map.setView(WORLD_VIEW, 2);
  }
}

export function renderTrail(points) {
  if (!map || !trailLayer) {
    return;
  }
  trailLayer.clearLayers();
  const grouped = new Map();
  for (const point of Array.isArray(points) ? points : []) {
    if (!point || !validCoordinatePair(point.latitude, point.longitude)) {
      continue;
    }
    const key = point.stable_key || "unknown";
    if (!grouped.has(key)) {
      grouped.set(key, []);
    }
    grouped.get(key).push(point);
  }
  for (const group of grouped.values()) {
    group.sort((left, right) => (left.observed_at || 0) - (right.observed_at || 0));
    const positions = group.map((point) => [point.latitude, point.longitude]);
    if (positions.length > 1) {
      window.L.polyline(positions, {
        color: "#54d8ec",
        dashArray: "2 7",
        weight: 3,
        opacity: 0.9,
        interactive: false,
      }).addTo(trailLayer);
    }
  }
  void TRAIL_LABEL;
}

export function focusEntity(stableKey) {
  if (!map || !activeByKey.has(stableKey)) {
    return false;
  }
  selectedKey = stableKey;
  updateSelectionAppearance();
  const entity = activeByKey.get(stableKey);
  map.panTo([entity.lat, entity.lon]);
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
  markerByKey = new Map();
  activeByKey = new Map();
  activeFingerprint = "";
  selectedKey = null;
  tileErrors = 0;
  tileSucceeded = false;
}
