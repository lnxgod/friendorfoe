import { element, formatCoordinates, hasNumber, replace, sourceLabel } from "../ui.js";

const HISTORY_LIMIT = 25;
const FILTER_ORDER = ["since", "until", "kind", "class", "source", "text", "positioned"];

function clone(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

export function buildHistoryParams(filters = {}, cursor = null, includePagination = true) {
  const params = new URLSearchParams();
  for (const name of FILTER_ORDER) {
    const value = filters[name];
    if (value === undefined || value === null || value === "") continue;
    if (name === "positioned") {
      if (typeof value === "boolean") params.set(name, value ? "true" : "false");
      continue;
    }
    params.set(name, String(value));
  }
  if (includePagination) {
    if (typeof cursor === "string" && cursor) params.set("cursor", cursor);
    params.set("limit", String(HISTORY_LIMIT));
  }
  return params;
}

function exportHref(path, filters) {
  const query = buildHistoryParams(filters, null, false).toString();
  return query ? `${path}?${query}` : path;
}

export function createHistoryController({ getHistory, post = async () => ({}) , onChange = () => {} }) {
  let active = false;
  let available = true;
  let filters = {};
  let cursor = null;
  let cursorStack = [];
  let items = [];
  let nextCursor = null;
  let phase = "idle";
  let message = "";
  let generation = 0;

  function snapshot() {
    return {
      active, available, filters: clone(filters), cursor,
      cursorStack: [...cursorStack], items: clone(items), nextCursor, phase, message,
    };
  }

  function notify() {
    onChange(snapshot());
  }

  async function load(navigation = null) {
    if (!active || !available) return false;
    const requestCursor = navigation ? navigation.cursor : cursor;
    const requestGeneration = ++generation;
    phase = "loading";
    message = "Loading history…";
    notify();
    try {
      const page = await getHistory(buildHistoryParams(filters, requestCursor, true));
      if (requestGeneration !== generation || !active) return false;
      if (navigation) {
        cursor = requestCursor;
        cursorStack = [...navigation.cursorStack];
      }
      items = Array.isArray(page?.items) ? page.items : [];
      nextCursor = typeof page?.next_cursor === "string" && page.next_cursor
        ? page.next_cursor : null;
      phase = items.length ? "ready" : "empty";
      message = items.length ? "" : "No history matches these filters.";
      notify();
      return true;
    } catch (error) {
      if (requestGeneration !== generation || !active) return false;
      phase = "error";
      message = error?.message || "History request failed.";
      notify();
      return false;
    }
  }

  return {
    snapshot,
    async activate(historyAvailable = true, unavailableMessage = "") {
      if (active) return false;
      active = true;
      available = historyAvailable !== false;
      if (!available) {
        phase = "unavailable";
        message = unavailableMessage || "History storage is unavailable.";
        notify();
        return false;
      }
      return load();
    },
    deactivate() {
      active = false;
      generation += 1;
    },
    observeAvailability(historyAvailable, unavailableMessage = "") {
      const wasAvailable = available;
      available = historyAvailable !== false;
      if (!available) {
        generation += 1;
        phase = "unavailable";
        message = unavailableMessage || "History storage is unavailable.";
        notify();
        return false;
      }
      if (active && !wasAvailable) return load();
      return false;
    },
    async applyFilters(nextFilters) {
      filters = { ...nextFilters };
      cursor = null;
      cursorStack = [];
      nextCursor = null;
      return load();
    },
    async next() {
      if (!nextCursor || phase === "loading") return false;
      return load({ cursor: nextCursor, cursorStack: [...cursorStack, cursor] });
    },
    async previous() {
      if (!cursorStack.length || phase === "loading") return false;
      return load({
        cursor: cursorStack.at(-1) ?? null,
        cursorStack: cursorStack.slice(0, -1),
      });
    },
    exportLinks() {
      return {
        csv: exportHref("/api/history/export.csv", filters),
        json: exportHref("/api/history/export.json", filters),
      };
    },
    async clear(typedConfirmation) {
      if (typedConfirmation !== "CLEAR" || phase === "loading") return false;
      try {
        await post("/api/history/clear", { confirm: "clear-history" });
        cursor = null;
        cursorStack = [];
        nextCursor = null;
        await load();
        return true;
      } catch (error) {
        phase = "error";
        message = error?.message || "History could not be cleared.";
        notify();
        return false;
      }
    },
  };
}

export function nextDialogFocus(elements, activeElement, shiftKey) {
  const enabled = elements.filter((item) => item && item.disabled !== true);
  if (!enabled.length) return null;
  const index = enabled.indexOf(activeElement);
  if (shiftKey) return index <= 0 ? enabled.at(-1) : enabled[index - 1];
  return index < 0 || index === enabled.length - 1 ? enabled[0] : enabled[index + 1];
}

function timeLabel(value) {
  if (!hasNumber(value)) return "Time missing";
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: "short", timeStyle: "medium",
  }).format(new Date(value * 1000));
}

function displayIdentity(item) {
  return item.display_id || item.label || item.stable_key || "Identity missing";
}

function numberLabel(value, suffix = "") {
  return hasNumber(value) ? `${value}${suffix}` : "—";
}

function confidenceLabel(value) {
  if (!hasNumber(value)) return "—";
  const percent = value >= 0 && value <= 1 ? value * 100 : value;
  return `${Math.round(percent * 10) / 10}%`;
}

function historyRow(item) {
  const row = element("tr");
  const kind = element("td");
  kind.append(element("strong", {
    className: item.kind === "track" ? "history-track" : "history-event",
    text: item.kind === "track" ? "Track" : "Event",
  }));
  if (item.kind === "track") {
    kind.append(element("span", { className: "truth-label", text: "Host-observed" }));
  }
  const position = item.latitude === null || item.latitude === undefined
    ? "—" : formatCoordinates(item.latitude, item.longitude);
  for (const cell of [
    timeLabel(item.observed_at ?? item.received_at), kind, displayIdentity(item),
    item.threat_class || "—", sourceLabel(item.source),
    confidenceLabel(item.confidence), numberLabel(item.score),
    numberLabel(item.rssi, " dBm"), position,
  ]) {
    row.append(cell instanceof Node ? cell : element("td", { text: cell }));
  }
  const detail = element("td");
  const facts = [];
  if (hasNumber(item.altitude_m)) facts.push(`Altitude ${item.altitude_m} m`);
  if (item.operator_id) facts.push(`Operator ${item.operator_id}`);
  if (hasNumber(item.events)) facts.push(`Events ${item.events}`);
  if (hasNumber(item.seen_count)) facts.push(`Seen ${item.seen_count}`);
  detail.textContent = facts.length ? facts.join(" · ") : "—";
  row.append(detail);
  return row;
}

function secondsFromLocalInput(value) {
  if (!value) return undefined;
  const milliseconds = Date.parse(value);
  return Number.isFinite(milliseconds) ? String(milliseconds / 1000) : undefined;
}

export function createHistoryView({ getHistory, post }) {
  const form = document.querySelector("#history-filters");
  const stateBox = document.querySelector("#history-state");
  const table = document.querySelector("#history-table");
  const body = document.querySelector("#history-results");
  const previous = document.querySelector("#history-previous");
  const next = document.querySelector("#history-next");
  const page = document.querySelector("#history-page");
  const csv = document.querySelector("#history-export-csv");
  const json = document.querySelector("#history-export-json");
  const clearOpen = document.querySelector("#history-clear-open");
  const dialog = document.querySelector("#history-clear-dialog");
  const clearInput = document.querySelector("#history-clear-word");
  const clearConfirm = document.querySelector("#history-clear-confirm");
  const clearCancel = document.querySelector("#history-clear-cancel");
  let restoreFocus = null;

  function render(state) {
    const visibleTable = state.phase === "ready";
    table.hidden = !visibleTable;
    stateBox.hidden = visibleTable;
    stateBox.className = `history-state ${state.phase}`;
    stateBox.textContent = state.message;
    replace(body, state.items.map(historyRow));
    previous.disabled = state.phase === "loading" || state.cursorStack.length === 0;
    next.disabled = state.phase === "loading" || !state.nextCursor;
    page.textContent = `Page ${state.cursorStack.length + 1}`;
    const links = controller.exportLinks();
    csv.href = links.csv;
    json.href = links.json;
  }

  const controller = createHistoryController({ getHistory, post, onChange: render });

  function readFilters() {
    const result = {};
    const since = secondsFromLocalInput(document.querySelector("#history-since").value);
    const until = secondsFromLocalInput(document.querySelector("#history-until").value);
    if (since !== undefined) result.since = since;
    if (until !== undefined) result.until = until;
    for (const [id, name] of [
      ["#history-kind", "kind"], ["#history-class", "class"],
      ["#history-source", "source"], ["#history-text", "text"],
    ]) {
      const value = document.querySelector(id).value.trim();
      if (value) result[name] = value;
    }
    if (document.querySelector("#history-positioned").checked) result.positioned = true;
    return result;
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    void controller.applyFilters(readFilters());
  });
  form.addEventListener("reset", () => window.setTimeout(() => void controller.applyFilters({}), 0));
  previous.addEventListener("click", () => void controller.previous());
  next.addEventListener("click", () => void controller.next());

  function closeDialog() {
    if (dialog.open) dialog.close();
  }
  clearOpen.addEventListener("click", () => {
    restoreFocus = document.activeElement;
    clearInput.value = "";
    clearConfirm.disabled = true;
    dialog.showModal();
    clearInput.focus();
  });
  clearInput.addEventListener("input", () => {
    clearConfirm.disabled = clearInput.value !== "CLEAR";
  });
  clearCancel.addEventListener("click", closeDialog);
  dialog.addEventListener("cancel", (event) => {
    event.preventDefault();
    closeDialog();
  });
  dialog.addEventListener("close", () => restoreFocus?.focus());
  dialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeDialog();
    } else if (event.key === "Tab") {
      const focusable = [clearInput, clearCancel, clearConfirm];
      const target = nextDialogFocus(focusable, document.activeElement, event.shiftKey);
      if (target && (document.activeElement === focusable[0] && event.shiftKey
        || document.activeElement === focusable.at(-1) && !event.shiftKey)) {
        event.preventDefault();
        target.focus();
      }
    }
  });
  clearConfirm.addEventListener("click", async () => {
    clearConfirm.disabled = true;
    if (await controller.clear(clearInput.value)) closeDialog();
  });

  render(controller.snapshot());
  return {
    activate(state) {
      const diagnostics = state?.diagnostics || {};
      return controller.activate(
        diagnostics.history_available !== false,
        diagnostics.history_error || "History storage is unavailable.",
      );
    },
    deactivate: () => controller.deactivate(),
    observeState(state) {
      const diagnostics = state?.diagnostics || {};
      controller.observeAvailability(
        diagnostics.history_available !== false,
        diagnostics.history_error || "History storage is unavailable.",
      );
    },
  };
}
