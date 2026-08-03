import {
  chip,
  definitionList,
  element,
  formatAge,
  formatCoordinates,
  formatInteger,
  formatNumber,
  hasNumber,
  replace,
  sourceLabel,
  stableEntityKey,
} from "../ui.js";

const REMOTE_ID_SOURCES = new Set(["ble_rid", "wifi_rid"]);
const NAMED_CLASS_FILTERS = new Set(["drone", "meta", "tracker", "wifi_attack"]);

function isRemoteId(entity) {
  return REMOTE_ID_SOURCES.has(entity?.source);
}

function matchesFilters(entity, state, filters) {
  const hostStale = state?.freshness?.state === "stale";
  if (!entity || entity.stale === true) {
    return false;
  }
  if (filters.class !== "all") {
    const entityClass = entity.class || "";
    if (filters.class === "other" ? NAMED_CLASS_FILTERS.has(entityClass) : entityClass !== filters.class) {
      return false;
    }
  }
  if (filters.source !== "all" && entity.source !== filters.source) {
    return false;
  }
  if (filters.minConfidence !== "all") {
    const minimum = Number(filters.minConfidence);
    if (!hasNumber(entity.confidence_pct) || entity.confidence_pct < minimum) {
      return false;
    }
  }
  if (filters.freshness === "live" && hostStale) {
    return false;
  }
  if (filters.freshness === "stale" && !hostStale) {
    return false;
  }
  return true;
}

function textValue(value) {
  return typeof value === "string" && value.trim() ? value : null;
}

function recentEventKey(event) {
  const badgeEntityKey = textValue(event.badge_entity_key);
  if (badgeEntityKey) {
    return `badge:${badgeEntityKey}`;
  }
  const source = textValue(event.source);
  const detectionId = textValue(event.detection_id);
  return source && detectionId ? `detection:${source}:${detectionId}` : null;
}

function confidencePercent(value) {
  if (!hasNumber(value)) {
    return undefined;
  }
  return value >= 0 && value <= 1 ? value * 100 : value;
}

export function recentUsbDetections(state, filters = null, nowSeconds = Date.now() / 1000) {
  const events = Array.isArray(state?.recent_events) ? state.recent_events : [];
  const seen = new Set();
  const detections = [];
  for (const event of events) {
    if (!event || typeof event !== "object" || Array.isArray(event)) {
      continue;
    }
    const key = recentEventKey(event);
    if (!key || seen.has(key)) {
      continue;
    }
    seen.add(key);
    const detectionId = textValue(event.detection_id);
    const label = textValue(event.badge_label)
      || textValue(event.manufacturer)
      || detectionId;
    const receivedAt = hasNumber(event.received_at) ? event.received_at : undefined;
    const lastSeen = hasNumber(receivedAt) && hasNumber(nowSeconds)
      ? Math.max(0, nowSeconds - receivedAt)
      : undefined;
    const detection = {
      display_id: detectionId,
      label,
      class: textValue(event.badge_class),
      source: textValue(event.source),
      confidence_pct: confidencePercent(event.confidence),
      score: hasNumber(event.threat_score) ? event.threat_score : undefined,
      rssi: hasNumber(event.rssi) ? event.rssi : undefined,
      last_seen_s: lastSeen,
      evidence: label ? `Recent native USB detection: ${label}.` : "Recent native USB detection.",
      recent_native_usb: true,
    };
    if (!filters || matchesFilters(detection, state, filters)) {
      detections.push(detection);
    }
  }
  return detections;
}

export function visibleEntities(state, filters) {
  const entities = Array.isArray(state?.status?.entities) ? state.status.entities : [];
  return entities.filter((entity) => matchesFilters(entity, state, filters));
}

export function groupVisibleEntities(state, filters) {
  const visible = visibleEntities(state, filters);
  const remoteId = visible.filter(isRemoteId);
  const droneClues = visible.filter((entity) => !isRemoteId(entity) && entity.class === "drone");
  const other = visible.filter((entity) => !isRemoteId(entity) && entity.class !== "drone");
  return { visible, remoteId, droneClues, other };
}

export function filteredRemoteIdKeys(state, filters) {
  return [...new Set(
    groupVisibleEntities(state, filters).remoteId.map((entity) => stableEntityKey(entity)),
  )].sort();
}

export function stateBanners(state) {
  const banners = [];
  const connection = state?.connection || {};
  const status = state?.status || {};
  const diagnostics = state?.diagnostics || {};
  const detail = connection.detail;
  const candidateText = Array.isArray(connection.candidates) && connection.candidates.length
    ? ` Candidates: ${connection.candidates.flat().join(", ")}.`
    : "";
  const messages = {
    no_badge: ["No badge — connect the uplink ESP32-S3 USB-C port.", "warning"],
    multiple_badges: [`Several badge ports were found.${candidateText} Restart with --port to select one.`, "warning"],
    explicit_port_missing: ["The selected badge port is missing. Check the USB cable and --port value.", "warning"],
    open_error: ["The badge port could not be opened. Close any flasher, serial monitor, or other New Dash instance.", "danger"],
    wrong_device: ["The selected USB device did not return a valid FOF_PONG. Check the uplink port and boot mode.", "danger"],
    read_error: ["The USB read failed. New Dash is retaining the last valid snapshot while reconnecting.", "warning"],
    serial_error: ["The USB session failed. New Dash is retaining the last valid snapshot while reconnecting.", "warning"],
    status_stale: ["Badge status is stale. The last host-observed snapshot remains visible and dimmed.", "warning"],
  };
  if (messages[detail]) {
    banners.push(messages[detail]);
  } else if (connection.phase === "reconnecting") {
    banners.push(["Reconnecting to the badge. The last valid snapshot remains visible.", "warning"]);
  } else if (["connecting", "verifying", "discovering"].includes(connection.phase)) {
    banners.push(["Verifying the badge USB connection.", "info"]);
  }
  if (state?.freshness?.state === "stale" && detail !== "status_stale") {
    banners.push([`Status is host-stale (${formatAge(state.freshness.age_s)}). Last valid data remains visible.`, "warning"]);
  }
  if (status.sensing_health === "safe_usb" || status.safe_mode === true || status.recovery_mode === "safe_usb") {
    banners.push(["Safe USB mode is active. USB controls may be healthy while RF scanner ingestion is disabled.", "warning"]);
  } else if (status.sensing_health === "degraded") {
    banners.push(["Scanner sensing is degraded. USB connectivity does not confirm healthy RF ingestion.", "warning"]);
  } else if (status.sensing_health === "unknown" && status.version) {
    banners.push(["Scanner health is not fully reported by this firmware snapshot.", "info"]);
  }
  if (!status.version) {
    banners.push(["No valid firmware status snapshot is available yet.", "info"]);
  }
  if (!Array.isArray(status.entities)) {
    const message = recentUsbDetections(state).length > 0
      ? "Active badge entity snapshot is temporarily unavailable. Recent native USB detections are shown separately."
      : "Active badge entity snapshot is temporarily unavailable. Waiting for native USB detections.";
    banners.push([message, "info"]);
  }
  if (diagnostics.history_available === false) {
    banners.push(["Local history is unavailable; observations are not being saved.", "danger"]);
  }
  if (hasNumber(diagnostics.persistence_drops) && diagnostics.persistence_drops > 0) {
    const drops = Math.round(diagnostics.persistence_drops);
    const noun = drops === 1 ? "observation" : "observations";
    banners.push([`Local history is incomplete: ${drops} ${noun} could not be saved.`, "danger"]);
  }
  return banners;
}

function renderBanners(state) {
  const stack = element("div", { className: "status-stack" });
  for (const [message, tone] of stateBanners(state)) {
    stack.append(element("p", { className: `state-banner ${tone}`, text: message }));
  }
  return stack.childElementCount ? stack : null;
}

function countRail(entities, available = true) {
  const remoteId = entities.filter(isRemoteId).length;
  const droneClues = entities.filter((entity) => !isRemoteId(entity) && entity.class === "drone").length;
  const other = Math.max(0, entities.length - remoteId - droneClues);
  const rail = element("div", { className: "count-rail", attributes: { "aria-label": "Active observation counts" } });
  for (const [label, value, variant] of [
    ["Remote ID", remoteId, "remote-id"],
    ["Drone clues", droneClues, "drone-clue"],
    ["Other", other, "other"],
  ]) {
    rail.append(element("div", { className: `count-item ${variant}` }, [
      element("span", { text: label }),
      element("strong", { text: available ? value : "—" }),
    ]));
  }
  return rail;
}

function firmwareCounts(status) {
  const section = element("section", { className: "observation-section firmware-counts", attributes: { "aria-label": "Firmware counts" } });
  section.append(element("div", { className: "section-heading" }, [element("h2", { text: "Firmware counts" })]));
  const counts = status?.counts;
  if (!counts || typeof counts !== "object" || Array.isArray(counts) || Object.keys(counts).length === 0) {
    section.append(element("p", { className: "empty-state", text: "Firmware counts missing." }));
    return section;
  }
  const facts = [];
  for (const [name, value] of Object.entries(counts)) {
    facts.push([name.replaceAll("_", " "), hasNumber(value) ? formatNumber(value) : "Missing"]);
  }
  section.append(definitionList(facts, "entity-facts"));
  return section;
}

function entityIdentity(entity) {
  return entity.display_id || entity.label || entity.bssid || entity.ssid || "Identity missing";
}

function truthSource(entity) {
  if (isRemoteId(entity)) {
    return sourceLabel(entity.source);
  }
  if (entity.source === "wifi_dji_ie") {
    return "DJI evidence — not Remote ID";
  }
  return sourceLabel(entity.source);
}

function renderEntity(entity) {
  const remoteId = isRemoteId(entity);
  const warningSource = entity.source === "wifi_dji_ie";
  const recentNativeUsb = entity.recent_native_usb === true;
  const record = element("article", { className: `entity-record${remoteId ? " remote-id" : ""}` });
  const primary = element("div", { className: "entity-primary" }, [
    element("p", {
      className: "entity-id",
      text: recentNativeUsb ? (entity.label || entityIdentity(entity)) : entityIdentity(entity),
    }),
    chip(truthSource(entity), remoteId ? "remote-id" : (warningSource ? "warning" : "")),
    element("span", { className: "entity-age", text: formatAge(entity.last_seen_s) }),
  ]);
  record.append(primary);
  const facts = [
    ["Class", entity.class || "Missing"],
    ["Confidence", formatNumber(entity.confidence_pct, "%")],
    ["Score", formatNumber(entity.score)],
    ["RSSI", formatInteger(entity.rssi, " dBm")],
  ];
  if (recentNativeUsb) {
    facts.unshift(["Detection ID", entity.display_id || "Missing"]);
  }
  record.append(definitionList(facts));
  const evidence = entity.evidence || entity.detail || "Evidence missing";
  record.append(element("p", {
    className: `evidence-line${warningSource ? " truth-warning" : ""}`,
    text: warningSource ? `DJI evidence — not Remote ID. ${evidence}` : evidence,
  }));
  if (remoteId && !recentNativeUsb) {
    record.append(definitionList([
      ["Drone position", formatCoordinates(entity.lat, entity.lon)],
      ["Altitude", formatNumber(entity.altitude_m, " m")],
      ["Operator position", formatCoordinates(entity.operator_lat, entity.operator_lon)],
      ["Operator identity", entity.operator_id || "Missing"],
      ["Manufacturer", entity.manufacturer || "Missing"],
      ["Seen count", formatInteger(entity.seen_count)],
    ], "location-facts"));
  }
  return record;
}

export function sectionCountLabel(count, unavailable = false) {
  if (unavailable) {
    return "Unavailable";
  }
  return `${count} ${count === 1 ? "item" : "items"}`;
}

function entitySection(title, entities, variant = "", unavailable = false) {
  const section = element("section", { className: "observation-section" });
  const heading = element("div", { className: `section-heading ${variant}` }, [
    element("h2", { text: title }),
    element("span", { text: sectionCountLabel(entities.length, unavailable) }),
  ]);
  section.append(heading);
  if (!entities.length) {
    const emptyText = unavailable
      ? "Active badge entity snapshot is temporarily unavailable."
      : `No ${title.toLowerCase()} match the current presentation filters.`;
    section.append(element("p", { className: "empty-state", text: emptyText }));
    return section;
  }
  const list = element("div", { className: "entity-list" });
  for (const entity of entities) {
    list.append(renderEntity(entity));
  }
  section.append(list);
  return section;
}

export function renderLive(root, state, filters) {
  const { visible, remoteId, droneClues, other } = groupVisibleEntities(state, filters);
  const recentDetections = recentUsbDetections(state, filters);
  const activeSnapshotAvailable = Array.isArray(state?.status?.entities);
  const children = [];
  const banners = renderBanners(state);
  if (banners) {
    children.push(banners);
  }
  children.push(countRail(visible, activeSnapshotAvailable));
  children.push(entitySection("Recent USB detections", recentDetections));
  children.push(firmwareCounts(state?.status));
  children.push(entitySection("Remote ID", remoteId, "remote-id", !activeSnapshotAvailable));
  children.push(entitySection("Drone clues", droneClues, "drone-clue", !activeSnapshotAvailable));
  children.push(entitySection("Other observations", other, "", !activeSnapshotAvailable));
  replace(root, children);
  root.closest(".view-panel")?.classList.toggle("is-host-stale", state?.freshness?.state === "stale");
}
