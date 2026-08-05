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
const LITE_IDENTITY = Object.freeze({
  product_family: "badge_lite",
  target: "uplink-s3-backend",
  project: "fof_backend_uplink",
  hardware: "seeed_xiao_esp32s3",
  mode: "headless",
});

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
  return {
    ok: true,
    value: {
      version: 1,
      palette: value.palette,
      background: value.background,
      brightness: value.brightness,
      accents: Object.fromEntries(ACCENTS.map((name) => [name, value.accents[name]])),
    },
  };
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
  return {
    ok: true,
    value: {
      version: 1,
      classes: Object.fromEntries(POLICY_CLASSES.map((name) => [name, {
        enabled: value.classes[name].enabled,
        lane: value.classes[name].lane,
        min_proximity: value.classes[name].min_proximity,
        priority: value.classes[name].priority,
      }])),
    },
  };
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

export function isTrustedHeadlessLite(status) {
  if (!isObject(status)) return false;
  const capabilities = status.capabilities;
  return Object.entries(LITE_IDENTITY).every(([key, value]) => status[key] === value)
    && Array.isArray(capabilities)
    && capabilities.includes("display_none");
}

export function readRequiredInteger(input) {
  const value = input?.value;
  if (typeof value !== "string" || value.trim() === "") return null;
  const parsed = Number(value);
  return Number.isInteger(parsed) ? parsed : null;
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
      const submitted = awaiting;
      const confirmed = Boolean(submitted && validated.ok && equal(validated.value, submitted));
      if (confirmed) awaiting = null;
      if (draft === null || (!dirty && !submitted)) {
        draft = validated.ok ? clone(validated.value) : null;
        dirty = false;
      } else {
        const validatedDraft = validate(draft);
        if (confirmed && validatedDraft.ok && equal(validatedDraft.value, submitted)) {
          draft = clone(validated.value);
          dirty = false;
        } else {
          dirty = !(validatedDraft.ok && current && equal(validatedDraft.value, current));
        }
      }
      return { confirmed };
    },
    edit(value) {
      draft = clone(value);
      const validated = validate(draft);
      dirty = !(validated.ok && current && equal(validated.value, current));
    },
    accept(value = draft) {
      const validated = validate(value);
      if (!validated.ok) return { confirmed: false };
      const confirmed = Boolean(current && equal(current, validated.value));
      awaiting = confirmed ? null : clone(validated.value);
      return { confirmed };
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
  let awaitingConfirmation = null;

  function snapshot() {
    return { pending, message, accepted, awaitingConfirmation };
  }
  function notify() {
    onChange(snapshot());
  }
  async function submit(path, body) {
    if (pending) return { accepted: false, message: "One command is already pending." };
    awaitingConfirmation = null;
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
      const submitted = clone(result.value);
      const reply = await submit(path, clone(submitted));
      if (reply.accepted) {
        awaitingConfirmation = path;
        notify();
        return { ...reply, submitted };
      }
      return reply;
    },
    confirm(path, labelText) {
      if (awaitingConfirmation !== path) return false;
      awaitingConfirmation = null;
      accepted = true;
      message = `${labelText} confirmed by firmware status.`;
      notify();
      return true;
    },
    snapshot,
  };
}

export function observeDraftStatus({ draft, mutations, path, label: labelText, value }) {
  const observation = draft.observe(value);
  if (observation.confirmed) mutations.confirm(path, labelText);
  return observation;
}

export async function submitDraft({ draft, mutations, path, label: labelText, validate }) {
  const reply = await mutations.submitValidated(path, draft.snapshot().draft, validate);
  if (!reply.accepted) return reply;
  const observation = draft.accept(reply.submitted);
  if (observation.confirmed) mutations.confirm(path, labelText);
  return reply;
}

function label(name) {
  return name.replaceAll("_", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function appendPresent(facts, source, key, display = label(key), suffix = "") {
  const value = presentFact(source, key);
  if (value !== null) facts.push([display, `${value}${suffix}`]);
}

const SCANNER_FIELDS = [
  ["slot", "Slot"], ["status_available", "Status available"],
  ["identity_valid", "Identity valid"], ["identity", "Identity"],
  ["uart", "UART"], ["role", "Role"], ["profile", "Profile"],
  ["connected", "Connected"], ["health", "Health"], ["firmware", "Firmware"],
  ["commands_sent", "Commands sent"], ["commands_accepted", "Commands accepted"],
  ["commands_failed", "Commands failed"], ["radio_events", "Radio events"],
  ["radio_packets", "Radio packets"], ["policy_ack", "Policy acknowledgement"],
  ["policy_hash", "Policy hash"], ["recovery", "Recovery"],
  ["recovery_mode", "Recovery mode"], ["ota", "OTA"], ["ota_state", "OTA state"],
  ["errors", "Errors"], ["uptime_ms", "Uptime (ms)"],
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

function renderHeadlessDiagnostics(root, status) {
  replace(root);
  const groups = [
    ["Identity", status, ["product_family", "target", "project", "hardware", "version", "mac", "boot_id", "config_generation", "led", "ota_ready"]],
    ["Wi-Fi", status.wifi, ["configured", "connected", "full_pass_failed"]],
    ["Recovery AP", status.recovery, ["reason", "ap_running"]],
    ["Backend", status.backend, ["reachable", "last_success_age_s"]],
    ["USB", status.usb, ["available", "host_connected", "required_depth", "optional_depth", "optional_drops", "required_failures", "bytes_transmitted", "bytes_received", "output_poisoned"]],
    ["Acknowledged live", status.live, ["started", "session_id", "last_ack_sequence", "confirmed", "lease_remaining_ms"]],
    ["Upload queue", status.upload, ["depth", "capacity", "dropped", "ok", "failed", "retries"]],
  ];
  for (const [title, source, keys] of groups) {
    if (!isObject(source)) continue;
    const facts = [];
    for (const key of keys) appendPresent(facts, source, key);
    if (!facts.length) continue;
    const section = element("section", { className: "headless-diagnostic-group" });
    section.append(element("h4", { text: title }), definitionList(facts, "diagnostic-facts"));
    root.append(section);
  }
  if (!root.children.length) {
    root.append(element("p", { className: "empty-state", text: "Headless diagnostics are unavailable." }));
  }
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

export function themeSnapshotFacts(theme) {
  const validated = validateTheme(theme);
  if (!validated.ok) return [];
  const current = validated.value;
  return [
    ["Version", String(current.version)],
    ["Palette", current.palette],
    ["Background", current.background],
    ["Brightness", `${current.brightness}%`],
    ...ACCENTS.map((name) => [`${name === "wifi_attack" ? "Wi-Fi attack" : label(name)} accent`, String(current.accents[name])]),
  ];
}

export function policySnapshotRows(policy) {
  const validated = validatePolicy(policy);
  if (!validated.ok) return [];
  return POLICY_CLASSES.map((name) => {
    const current = validated.value.classes[name];
    return [
      name, current.enabled ? "Yes" : "No", current.lane,
      current.min_proximity, String(current.priority),
    ];
  });
}

function renderCurrentTheme(root, theme) {
  const facts = themeSnapshotFacts(theme);
  replace(root, facts.length
    ? [definitionList(facts, "diagnostic-facts current-theme-facts")]
    : [element("p", { className: "empty-state", text: "Current firmware theme unavailable." })]);
}

function renderCurrentPolicy(root, policy) {
  const rows = policySnapshotRows(policy);
  if (!rows.length) {
    replace(root, [element("p", {
      className: "empty-state", text: "Current firmware display policy unavailable.",
    })]);
    return;
  }
  const table = element("table", { className: "current-policy-table" });
  const head = element("thead");
  const heading = element("tr");
  for (const text of ["Class", "Enabled", "Lane", "Minimum proximity", "Priority"]) {
    heading.append(element("th", { text }));
  }
  head.append(heading);
  const body = element("tbody");
  for (const values of rows) {
    const row = element("tr");
    values.forEach((text, index) => row.append(element(index === 0 ? "th" : "td", { text })));
    body.append(row);
  }
  table.append(head, body);
  replace(root, [element("p", { className: "snapshot-version", text: "Version 1" }), table]);
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
    brightness: readRequiredInteger(document.querySelector("#theme-brightness")),
    accents: Object.fromEntries(ACCENTS.map((name) => [
      name, readRequiredInteger(document.querySelector(`#theme-accent-${name}`)),
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
      priority: readRequiredInteger(row.querySelector("[data-policy-field=priority]")),
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
  const policyCurrent = document.querySelector("#badge-policy-current");
  const headlessSection = document.querySelector("#badge-headless-section");
  const headlessRoot = document.querySelector("#badge-headless-diagnostics");
  const displayOnly = [...document.querySelectorAll("[data-display-only]")];
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
    await submitDraft({
      draft: themeDraft, mutations, path: "/api/control/theme", label: "Theme",
      validate: validateTheme,
    });
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
    await submitDraft({
      draft: policyDraft, mutations, path: "/api/control/display-policy",
      label: "Display policy", validate: validatePolicy,
    });
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
      const headless = isTrustedHeadlessLite(status);
      displayOnly.forEach((node) => { node.hidden = headless; });
      headlessSection.hidden = !headless;
      canMutate = !headless && state?.connection?.phase === "live" && state?.freshness?.state === "fresh";
      renderScanners(
        scannerRoot,
        headless ? status.scanner_summaries || status.scanner : status.scanners,
        status.sensing_health,
      );
      renderStatusFacts(statusRoot, status);
      if (headless) renderHeadlessDiagnostics(headlessRoot, status);
      renderDisplayState(displayRoot, status.display_state);
      observeDraftStatus({
        draft: themeDraft, mutations, path: "/api/control/theme", label: "Theme", value: status.theme,
      });
      observeDraftStatus({
        draft: policyDraft, mutations, path: "/api/control/display-policy",
        label: "Display policy", value: status.display_policy,
      });
      const theme = themeDraft.snapshot();
      const policy = policyDraft.snapshot();
      renderCurrentTheme(themeCurrent, theme.current);
      renderCurrentPolicy(policyCurrent, policy.current);
      setThemeForm(theme.draft);
      setPolicyForm(policy.draft);
      document.querySelector("#theme-unavailable").hidden = theme.complete;
      document.querySelector("#policy-unavailable").hidden = policy.complete;
      syncDisabled();
    },
  };
}
