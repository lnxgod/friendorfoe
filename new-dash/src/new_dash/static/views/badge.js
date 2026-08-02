import { definitionList, element, replace } from "../ui.js";

export const POLICY_CLASSES = Object.freeze([
  "drone", "meta", "tracker", "wifi_attack", "skimmer", "camera", "flock",
  "lock", "hid", "beacon", "event_badge", "auracast", "scanner_status",
]);
const POLICY_CLASS_SET = new Set(POLICY_CLASSES);
const PALETTES = new Set(["field", "night", "neon", "mono"]);
const BACKGROUNDS = new Set(["dark", "dim", "scanline"]);
const ACCENTS = ["drone", "meta", "tracker", "flock", "wifi_attack", "clear"];
const LANES = new Set(["off", "lower", "top", "both"]);
const PROXIMITIES = new Set(["present", "near", "close"]);
const NAV_ACTIONS = new Set(["next", "detail", "page", "back"]);

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function exactKeys(value, expected) {
  if (!isObject(value)) return false;
  const keys = Object.keys(value);
  return keys.length === expected.length && keys.every((key) => expected.includes(key));
}

function clone(value) {
  return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
}

function equal(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

export function validateTheme(value) {
  if (!exactKeys(value, ["version", "palette", "background", "brightness", "accents"])) {
    return { ok: false, message: "A complete version-1 firmware theme is required." };
  }
  if (value.version !== 1 || !PALETTES.has(value.palette) || !BACKGROUNDS.has(value.background)
      || !Number.isInteger(value.brightness) || value.brightness < 25 || value.brightness > 100
      || !exactKeys(value.accents, ACCENTS)) {
    return { ok: false, message: "Theme fields are incomplete or outside firmware ranges." };
  }
  if (ACCENTS.some((name) => !Number.isInteger(value.accents[name])
      || value.accents[name] < 0 || value.accents[name] > 65535)) {
    return { ok: false, message: "Theme accents must be RGB565 integers from 0 through 65535." };
  }
  return { ok: true, value: clone(value) };
}

export function validatePolicy(value) {
  if (!exactKeys(value, ["version", "classes"]) || value.version !== 1 || !isObject(value.classes)) {
    return { ok: false, message: "A complete version-1 firmware display policy is required." };
  }
  const names = Object.keys(value.classes);
  if (names.length !== POLICY_CLASSES.length || names.some((name) => !POLICY_CLASS_SET.has(name))) {
    return { ok: false, message: "Display policy must contain exactly the 13 firmware classes." };
  }
  for (const name of POLICY_CLASSES) {
    const policy = value.classes[name];
    if (!exactKeys(policy, ["enabled", "lane", "min_proximity", "priority"])
        || typeof policy.enabled !== "boolean" || !LANES.has(policy.lane)
        || !PROXIMITIES.has(policy.min_proximity) || !Number.isInteger(policy.priority)
        || policy.priority < 0 || policy.priority > 100
        || (!policy.enabled && policy.lane !== "off")
        || (policy.enabled && policy.lane === "off")) {
      return { ok: false, message: `Display policy class ${name} is invalid.` };
    }
  }
  return { ok: true, value: clone(value) };
}

export function navigationPayload(action) {
  if (!NAV_ACTIONS.has(action)) throw new Error("Invalid display navigation action.");
  return { action };
}

export function presentFact(source, key) {
  if (!isObject(source) || !Object.hasOwn(source, key) || source[key] === null || source[key] === undefined) {
    return null;
  }
  const value = source[key];
  if (typeof value === "boolean") return value ? "Yes" : "No";
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

export function applyAllowed({ canMutate, pending, draft }) {
  return canMutate === true && pending !== true && draft?.complete === true
    && draft.valid === true && draft.dirty === true && !draft.awaiting;
}

export function createDraftState(validate) {
  let current = null;
  let draft = null;
  let complete = false;
  let dirty = false;
  let awaiting = null;

  return {
    observe(value) {
      const validated = validate(value);
      current = validated.ok ? validated.value : null;
      complete = validated.ok;
      if (awaiting && validated.ok && equal(validated.value, awaiting)) {
        dirty = false;
        awaiting = null;
        draft = clone(validated.value);
      } else if (!dirty) {
        draft = validated.ok ? clone(validated.value) : null;
      }
    },
    edit(value) {
      draft = clone(value);
      dirty = true;
    },
    accept() {
      if (dirty && validate(draft).ok) awaiting = clone(draft);
    },
    snapshot() {
      return {
        current: clone(current), draft: clone(draft), complete,
        dirty, awaiting: clone(awaiting), valid: validate(draft).ok,
      };
    },
  };
}

function propagationMessage(reply) {
  const parts = ["Accepted; awaiting status"];
  if (Object.hasOwn(reply || {}, "ble_sent")) parts.push(reply.ble_sent ? "BLE sent" : "BLE not sent");
  if (Object.hasOwn(reply || {}, "wifi_sent")) parts.push(reply.wifi_sent ? "Wi-Fi sent" : "Wi-Fi not sent");
  return parts.join(" · ");
}

export function createMutationCoordinator({ post, onChange = () => {} }) {
  let pending = false;
  let message = "";
  let accepted = false;

  function snapshot() {
    return { pending, message, accepted };
  }
  function notify() {
    onChange(snapshot());
  }
  async function submit(path, body) {
    if (pending) return { accepted: false, message: "One command is already pending." };
    pending = true;
    accepted = false;
    message = "Command pending…";
    notify();
    try {
      const reply = await post(path, body);
      if (reply?.ok !== true) throw new Error(reply?.error || "Firmware did not accept the command.");
      accepted = true;
      message = propagationMessage(reply);
      return { accepted: true, message, reply };
    } catch (error) {
      accepted = false;
      message = error?.message || "Command failed.";
      return { accepted: false, message, error };
    } finally {
      pending = false;
      notify();
    }
  }
  return {
    submit,
    async submitValidated(path, body, validate) {
      const result = validate(body);
      if (!result.ok) {
        accepted = false;
        message = result.message;
        notify();
        return { accepted: false, message };
      }
      return submit(path, result.value);
    },
    snapshot,
  };
}

function label(name) {
  return name.replaceAll("_", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function appendPresent(facts, source, key, display = label(key), suffix = "") {
  const value = presentFact(source, key);
  if (value !== null) facts.push([display, `${value}${suffix}`]);
}

const SCANNER_FIELDS = [
  ["uart", "UART"], ["role", "Role"], ["profile", "Profile"],
  ["connected", "Connected"], ["health", "Health"], ["firmware", "Firmware"],
  ["commands_sent", "Commands sent"], ["commands_accepted", "Commands accepted"],
  ["commands_failed", "Commands failed"], ["radio_events", "Radio events"],
  ["radio_packets", "Radio packets"], ["policy_ack", "Policy acknowledgement"],
  ["policy_hash", "Policy hash"], ["recovery", "Recovery"],
  ["recovery_mode", "Recovery mode"], ["ota", "OTA"], ["ota_state", "OTA state"],
];
const SCANNER_KNOWN = new Set(SCANNER_FIELDS.map(([key]) => key));

function extrasDetails(source, known, title = "Additional firmware facts") {
  const entries = Object.entries(source || {}).filter(([key, value]) => !known.has(key)
    && value !== undefined && value !== null);
  if (!entries.length) return null;
  const details = element("details", { className: "diagnostic-extras" });
  details.append(element("summary", { text: title }));
  const list = element("dl", { className: "diagnostic-facts" });
  for (const [key, value] of entries) {
    const item = element("div");
    item.append(element("dt", { text: label(key) }), element("dd", {
      text: typeof value === "object" ? JSON.stringify(value) : String(value),
    }));
    list.append(item);
  }
  details.append(list);
  return details;
}

function renderScanners(root, scanners, sensingHealth) {
  replace(root);
  if (sensingHealth === "safe_usb") {
    root.append(element("p", {
      className: "diagnostic-note warning",
      text: "USB control is healthy; scanner sensing is unavailable in safe USB mode.",
    }));
  }
  if (!Array.isArray(scanners) || !scanners.length) {
    root.append(element("p", { className: "empty-state", text: sensingHealth === "safe_usb"
      ? "No scanner status is expected while safe USB mode disables sensing."
      : "Scanner diagnostics are unavailable." }));
    return;
  }
  for (const [index, scanner] of scanners.entries()) {
    const section = element("section", { className: "scanner-record" });
    section.append(element("h4", { text: scanner.role || scanner.uart || `Scanner ${index + 1}` }));
    const facts = [];
    for (const [key, display] of SCANNER_FIELDS) appendPresent(facts, scanner, key, display);
    section.append(definitionList(facts, "diagnostic-facts"));
    const extras = extrasDetails(scanner, SCANNER_KNOWN);
    if (extras) section.append(extras);
    root.append(section);
  }
}

const STATUS_KNOWN = new Set([
  "version", "uptime_s", "mode", "mode_label", "safe_mode", "safe_reason",
  "recovery_mode", "reset_reason", "reset_count", "last_reset", "reporting",
  "memory", "stack", "stack_free", "stack_high_water", "heap", "heap_free",
  "heap_total", "psram", "psram_free", "psram_total", "counts", "scanners",
  "entities", "remote_id_entities", "display_state", "theme", "display_policy",
  "sensing_health", "threat_score",
]);

function renderStatusFacts(root, status) {
  const facts = [];
  for (const [key, display, suffix] of [
    ["version", "Firmware", ""], ["uptime_s", "Uptime", " s"],
    ["mode_label", "Mode", ""], ["mode", "Mode code", ""],
    ["safe_mode", "Safe mode", ""], ["safe_reason", "Safe reason", ""],
    ["recovery_mode", "Recovery mode", ""], ["reset_reason", "Reset reason", ""],
    ["reset_count", "Reset count", ""], ["last_reset", "Last reset", ""],
    ["stack", "Stack", ""], ["stack_free", "Stack free", ""],
    ["stack_high_water", "Stack high-water", ""], ["heap", "Heap", ""],
    ["heap_free", "Heap free", ""], ["heap_total", "Heap total", ""],
    ["psram", "PSRAM", ""], ["psram_free", "PSRAM free", ""],
    ["psram_total", "PSRAM total", ""],
  ]) appendPresent(facts, status, key, display, suffix);
  for (const [prefix, source] of [["Reporting", status.reporting], ["Memory", status.memory], ["Counts", status.counts]]) {
    if (!isObject(source)) continue;
    for (const key of Object.keys(source)) appendPresent(facts, source, key, `${prefix} ${label(key)}`);
  }
  replace(root, [definitionList(facts, "diagnostic-facts")]);
  const extras = extrasDetails(status, STATUS_KNOWN);
  if (extras) root.append(extras);
}

function renderDisplayState(root, displayState) {
  if (!isObject(displayState)) {
    replace(root, [element("p", { className: "empty-state", text: "Display state unavailable." })]);
    return;
  }
  const facts = Object.entries(displayState).filter(([, value]) => value !== null && value !== undefined)
    .map(([key, value]) => [label(key), typeof value === "object" ? JSON.stringify(value) : String(value)]);
  replace(root, [definitionList(facts, "diagnostic-facts")]);
}

function formatTheme(theme) {
  if (!theme) return "Current firmware theme unavailable.";
  return `${theme.palette} · ${theme.background} · ${theme.brightness}% · RGB565 accents`;
}

function setThemeForm(theme) {
  if (!theme) {
    document.querySelector("#theme-palette").value = "";
    document.querySelector("#theme-background").value = "";
    document.querySelector("#theme-brightness").value = "";
    for (const name of ACCENTS) document.querySelector(`#theme-accent-${name}`).value = "";
    return;
  }
  document.querySelector("#theme-palette").value = theme.palette;
  document.querySelector("#theme-background").value = theme.background;
  document.querySelector("#theme-brightness").value = String(theme.brightness);
  for (const name of ACCENTS) document.querySelector(`#theme-accent-${name}`).value = String(theme.accents[name]);
}

function readThemeForm() {
  return {
    version: 1,
    palette: document.querySelector("#theme-palette").value,
    background: document.querySelector("#theme-background").value,
    brightness: Number(document.querySelector("#theme-brightness").value),
    accents: Object.fromEntries(ACCENTS.map((name) => [
      name, Number(document.querySelector(`#theme-accent-${name}`).value),
    ])),
  };
}

function setPolicyForm(policy) {
  if (!policy) {
    for (const name of POLICY_CLASSES) {
      const row = document.querySelector(`[data-policy-class="${name}"]`);
      row.querySelector("[data-policy-field=enabled]").checked = false;
      row.querySelector("[data-policy-field=lane]").value = "";
      row.querySelector("[data-policy-field=min_proximity]").value = "";
      row.querySelector("[data-policy-field=priority]").value = "";
    }
    return;
  }
  for (const name of POLICY_CLASSES) {
    const row = document.querySelector(`[data-policy-class="${name}"]`);
    const value = policy.classes[name];
    row.querySelector("[data-policy-field=enabled]").checked = value.enabled;
    row.querySelector("[data-policy-field=lane]").value = value.lane;
    row.querySelector("[data-policy-field=min_proximity]").value = value.min_proximity;
    row.querySelector("[data-policy-field=priority]").value = String(value.priority);
  }
}

function readPolicyForm() {
  const classes = {};
  for (const name of POLICY_CLASSES) {
    const row = document.querySelector(`[data-policy-class="${name}"]`);
    classes[name] = {
      enabled: row.querySelector("[data-policy-field=enabled]").checked,
      lane: row.querySelector("[data-policy-field=lane]").value,
      min_proximity: row.querySelector("[data-policy-field=min_proximity]").value,
      priority: Number(row.querySelector("[data-policy-field=priority]").value),
    };
  }
  return { version: 1, classes };
}

function installConfirmationDialog(dialog, openButton, cancelButton, confirmButton, onConfirm) {
  let restoreFocus = null;
  openButton.addEventListener("click", () => {
    restoreFocus = document.activeElement;
    dialog.showModal();
    cancelButton.focus();
  });
  cancelButton.addEventListener("click", () => dialog.close());
  dialog.addEventListener("cancel", (event) => {
    event.preventDefault();
    dialog.close();
  });
  dialog.addEventListener("close", () => restoreFocus?.focus());
  dialog.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      dialog.close();
    } else if (event.key === "Tab") {
      const controls = [cancelButton, confirmButton].filter((control) => !control.disabled);
      const current = controls.indexOf(document.activeElement);
      if ((!event.shiftKey && current === controls.length - 1) || (event.shiftKey && current === 0)) {
        event.preventDefault();
        controls[event.shiftKey ? controls.length - 1 : 0]?.focus();
      }
    }
  });
  confirmButton.addEventListener("click", async () => {
    if (await onConfirm()) dialog.close();
  });
}

export function createBadgeView({ post }) {
  const scannerRoot = document.querySelector("#badge-scanners");
  const statusRoot = document.querySelector("#badge-status-facts");
  const displayRoot = document.querySelector("#badge-display-state");
  const themeCurrent = document.querySelector("#badge-theme-current");
  const themeForm = document.querySelector("#badge-theme-form");
  const policyForm = document.querySelector("#badge-policy-form");
  const themeApply = document.querySelector("#theme-apply");
  const policyApply = document.querySelector("#policy-apply");
  const result = document.querySelector("#badge-command-result");
  const themeDraft = createDraftState(validateTheme);
  const policyDraft = createDraftState(validatePolicy);
  let canMutate = false;

  function syncDisabled() {
    const pending = mutations.snapshot().pending;
    for (const control of document.querySelectorAll("[data-mutation]")) {
      control.disabled = !canMutate || pending;
    }
    const theme = themeDraft.snapshot();
    const policy = policyDraft.snapshot();
    themeApply.disabled = !applyAllowed({ canMutate, pending, draft: theme });
    policyApply.disabled = !applyAllowed({ canMutate, pending, draft: policy });
  }

  const mutations = createMutationCoordinator({
    post,
    onChange(state) {
      result.hidden = !state.message;
      result.className = `command-result ${state.pending ? "pending" : state.accepted ? "accepted" : "rejected"}`;
      result.textContent = state.message;
      syncDisabled();
    },
  });

  for (const button of document.querySelectorAll("[data-nav-action]")) {
    button.addEventListener("click", () => {
      let payload;
      try { payload = navigationPayload(button.dataset.navAction); } catch (error) { return; }
      void mutations.submit("/api/control/display-nav", payload);
    });
  }

  themeForm.addEventListener("input", () => {
    themeDraft.edit(readThemeForm());
    syncDisabled();
  });
  themeForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const reply = await mutations.submitValidated("/api/control/theme", themeDraft.snapshot().draft, validateTheme);
    if (reply.accepted) themeDraft.accept();
    syncDisabled();
  });
  policyForm.addEventListener("input", () => {
    policyDraft.edit(readPolicyForm());
    syncDisabled();
  });
  policyForm.addEventListener("change", () => {
    policyDraft.edit(readPolicyForm());
    syncDisabled();
  });
  policyForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const reply = await mutations.submitValidated(
      "/api/control/display-policy", policyDraft.snapshot().draft, validatePolicy,
    );
    if (reply.accepted) policyDraft.accept();
    syncDisabled();
  });

  installConfirmationDialog(
    document.querySelector("#theme-reset-dialog"), document.querySelector("#theme-reset-open"),
    document.querySelector("#theme-reset-cancel"), document.querySelector("#theme-reset-confirm"),
    async () => (await mutations.submit("/api/control/theme/reset", {})).accepted,
  );
  installConfirmationDialog(
    document.querySelector("#policy-reset-dialog"), document.querySelector("#policy-reset-open"),
    document.querySelector("#policy-reset-cancel"), document.querySelector("#policy-reset-confirm"),
    async () => (await mutations.submit("/api/control/display-policy/reset", {})).accepted,
  );

  return {
    render(state) {
      const status = state?.status || {};
      canMutate = state?.connection?.phase === "live" && state?.freshness?.state === "fresh";
      renderScanners(scannerRoot, status.scanners, status.sensing_health);
      renderStatusFacts(statusRoot, status);
      renderDisplayState(displayRoot, status.display_state);
      themeDraft.observe(status.theme);
      policyDraft.observe(status.display_policy);
      const theme = themeDraft.snapshot();
      const policy = policyDraft.snapshot();
      themeCurrent.textContent = formatTheme(theme.current);
      setThemeForm(theme.draft);
      setPolicyForm(policy.draft);
      document.querySelector("#theme-unavailable").hidden = theme.complete;
      document.querySelector("#policy-unavailable").hidden = policy.complete;
      syncDisabled();
    },
  };
}
