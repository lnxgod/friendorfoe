const NUMBER_FORMAT = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 });
const COORDINATE_FORMAT = new Intl.NumberFormat(undefined, {
  minimumFractionDigits: 4,
  maximumFractionDigits: 6,
});

export function element(tagName, options = {}, children = []) {
  const node = document.createElement(tagName);
  if (options.className) {
    node.className = options.className;
  }
  if (options.text !== undefined && options.text !== null) {
    node.textContent = String(options.text);
  }
  for (const [name, value] of Object.entries(options.attributes || {})) {
    if (value !== undefined && value !== null) {
      node.setAttribute(name, String(value));
    }
  }
  const childList = Array.isArray(children) ? children : [children];
  for (const child of childList) {
    if (child instanceof Node) {
      node.append(child);
    }
  }
  return node;
}

export function chip(text, variant = "") {
  const className = ["source-chip", variant].filter(Boolean).join(" ");
  return element("span", { className, text });
}

export function replace(root, children = []) {
  root.replaceChildren(...children);
}

export function hasNumber(value) {
  return typeof value === "number" && Number.isFinite(value);
}

export function formatNumber(value, suffix = "") {
  return hasNumber(value) ? `${NUMBER_FORMAT.format(value)}${suffix}` : "Missing";
}

export function formatInteger(value, suffix = "") {
  return hasNumber(value) ? `${Math.round(value)}${suffix}` : "Missing";
}

export function formatAge(value) {
  return hasNumber(value) ? `${NUMBER_FORMAT.format(Math.max(0, value))} s ago` : "Age missing";
}

export function validCoordinatePair(latitude, longitude) {
  return hasNumber(latitude)
    && hasNumber(longitude)
    && latitude >= -90
    && latitude <= 90
    && longitude >= -180
    && longitude <= 180;
}

export function formatCoordinates(latitude, longitude) {
  if (!validCoordinatePair(latitude, longitude)) {
    return "Missing";
  }
  return `${COORDINATE_FORMAT.format(latitude)}, ${COORDINATE_FORMAT.format(longitude)}`;
}

export function sourceLabel(source) {
  const labels = {
    ble_rid: "BLE Remote ID",
    wifi_rid: "Wi-Fi Remote ID",
    wifi_dji_ie: "DJI vendor IE",
    wifi_ssid: "Wi-Fi SSID",
    wifi_oui: "Wi-Fi OUI",
    wifi_probe: "Wi-Fi probe",
    ble_fingerprint: "BLE fingerprint",
    wifi_assoc: "Wi-Fi association",
    wifi_inventory: "Wi-Fi inventory",
    unknown: "Unknown source",
  };
  return labels[source] || (source ? String(source) : "Source missing");
}

export function scannerSummary(status) {
  if (!Array.isArray(status?.scanners)) {
    return "Scanners unavailable";
  }
  const count = status.scanners.length;
  const health = typeof status.sensing_health === "string" && status.sensing_health
    ? ` · ${status.sensing_health.replaceAll("_", " ")}`
    : "";
  return `${count} ${count === 1 ? "scanner" : "scanners"}${health}`;
}

export function nextTabIndex(current, key, count) {
  if (!Number.isInteger(current) || !Number.isInteger(count) || count < 1) {
    return null;
  }
  if (key === "ArrowLeft") {
    return (current - 1 + count) % count;
  }
  if (key === "ArrowRight") {
    return (current + 1) % count;
  }
  if (key === "Home") {
    return 0;
  }
  if (key === "End") {
    return count - 1;
  }
  return null;
}

export function stableEntityKey(entity) {
  if (typeof entity?.stable_key === "string" && entity.stable_key) {
    return entity.stable_key;
  }
  const identity = entity?.display_id || entity?.bssid || entity?.ssid || entity?.label || "unknown";
  return `${entity?.source || "unknown"}:${identity}`;
}

export function definitionList(facts, className = "entity-facts") {
  const list = element("dl", { className });
  for (const [label, value] of facts) {
    const item = element("div");
    item.append(
      element("dt", { text: label }),
      element("dd", { text: value === undefined || value === null || value === "" ? "Missing" : value }),
    );
    list.append(item);
  }
  return list;
}

export function readPreference(key, validate, fallback) {
  try {
    const stored = window.localStorage.getItem(key);
    if (stored === null) {
      return fallback;
    }
    const parsed = JSON.parse(stored);
    return validate(parsed) ? parsed : fallback;
  } catch (_error) {
    return fallback;
  }
}

export function writePreference(key, value) {
  try {
    window.localStorage.setItem(key, JSON.stringify(value));
  } catch (_error) {
    // The dashboard remains usable when storage is disabled or full.
  }
}
