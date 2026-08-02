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
} from "../ui.js";

const REMOTE_ID_SOURCES = new Set(["ble_rid", "wifi_rid"]);

function isRemoteId(entity) {
  return REMOTE_ID_SOURCES.has(entity?.source);
}

export function visibleEntities(state, filters) {
  const hostStale = state?.freshness?.state === "stale";
  const entities = Array.isArray(state?.status?.entities) ? state.status.entities : [];
  return entities.filter((entity) => {
    if (!entity || entity.stale === true) {
      return false;
    }
    if (filters.class !== "all") {
      const entityClass = entity.class || "other";
      if (filters.class === "other" ? entityClass !== "other" : entityClass !== filters.class) {
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
  });
}

function stateBanners(state) {
  const banners = [];
  const connection = state?.connection || {};
  const status = state?.status || {};
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
  return banners;
}

function renderBanners(state) {
  const stack = element("div", { className: "status-stack" });
  for (const [message, tone] of stateBanners(state)) {
    stack.append(element("p", { className: `state-banner ${tone}`, text: message }));
  }
  return stack.childElementCount ? stack : null;
}

function countRail(entities) {
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
      element("strong", { text: value }),
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
  const record = element("article", { className: `entity-record${remoteId ? " remote-id" : ""}` });
  const primary = element("div", { className: "entity-primary" }, [
    element("p", { className: "entity-id", text: entityIdentity(entity) }),
    chip(truthSource(entity), remoteId ? "remote-id" : (warningSource ? "warning" : "")),
    element("span", { className: "entity-age", text: formatAge(entity.last_seen_s) }),
  ]);
  record.append(primary);
  record.append(definitionList([
    ["Class", entity.class || "Missing"],
    ["Confidence", formatNumber(entity.confidence_pct, "%")],
    ["Score", formatNumber(entity.score)],
    ["RSSI", formatInteger(entity.rssi, " dBm")],
  ]));
  const evidence = entity.evidence || entity.detail || "Evidence missing";
  record.append(element("p", {
    className: `evidence-line${warningSource ? " truth-warning" : ""}`,
    text: warningSource ? `DJI evidence — not Remote ID. ${evidence}` : evidence,
  }));
  if (remoteId) {
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

function entitySection(title, entities, variant = "") {
  const section = element("section", { className: "observation-section" });
  const heading = element("div", { className: `section-heading ${variant}` }, [
    element("h2", { text: title }),
    element("span", { text: `${entities.length} ${entities.length === 1 ? "item" : "items"}` }),
  ]);
  section.append(heading);
  if (!entities.length) {
    section.append(element("p", { className: "empty-state", text: `No ${title.toLowerCase()} match the current presentation filters.` }));
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
  const active = (Array.isArray(state?.status?.entities) ? state.status.entities : [])
    .filter((entity) => entity && entity.stale !== true);
  const visible = visibleEntities(state, filters);
  const remoteId = visible.filter(isRemoteId);
  const droneClues = visible.filter((entity) => !isRemoteId(entity) && entity.class === "drone");
  const other = visible.filter((entity) => !isRemoteId(entity) && entity.class !== "drone");
  const children = [];
  const banners = renderBanners(state);
  if (banners) {
    children.push(banners);
  }
  children.push(countRail(active));
  children.push(firmwareCounts(state?.status));
  children.push(entitySection("Remote ID", remoteId, "remote-id"));
  children.push(entitySection("Drone clues", droneClues, "drone-clue"));
  children.push(entitySection("Other observations", other));
  replace(root, children);
  root.closest(".view-panel")?.classList.toggle("is-host-stale", state?.freshness?.state === "stale");
}
