const REQUEST_TIMEOUT_MS = 5000;

export class ApiError extends Error {
  constructor(code, message, status = 0) {
    super(message);
    this.name = "ApiError";
    this.code = code;
    this.status = status;
  }
}

async function request(path, options = {}) {
  const controller = new AbortController();
  const externalSignal = options.signal;
  let timeoutExpired = false;
  const abortFromExternal = () => controller.abort();
  if (externalSignal?.aborted) {
    controller.abort();
  } else {
    externalSignal?.addEventListener("abort", abortFromExternal, { once: true });
  }
  const timeout = window.setTimeout(() => {
    timeoutExpired = true;
    controller.abort();
  }, REQUEST_TIMEOUT_MS);
  try {
    const response = await fetch(path, {
      ...options,
      credentials: "same-origin",
      signal: controller.signal,
    });
    let envelope;
    try {
      envelope = await response.json();
    } catch (_error) {
      throw new ApiError("invalid_response", "New Dash returned an invalid response.", response.status);
    }
    if (!response.ok || envelope?.ok !== true) {
      const code = envelope?.error?.code || "request_failed";
      const message = envelope?.error?.message || `New Dash request failed (${response.status}).`;
      throw new ApiError(code, message, response.status);
    }
    return envelope.data;
  } catch (error) {
    if (error instanceof ApiError) {
      throw error;
    }
    if (error?.name === "AbortError" && externalSignal?.aborted && !timeoutExpired) {
      throw new ApiError("request_aborted", "New Dash request was cancelled.");
    }
    if (error?.name === "AbortError") {
      throw new ApiError("request_timeout", "New Dash did not respond within five seconds.");
    }
    throw new ApiError("network_error", "New Dash could not be reached.");
  } finally {
    window.clearTimeout(timeout);
    externalSignal?.removeEventListener("abort", abortFromExternal);
  }
}

export function getState(options = {}) {
  return request("/api/state", options);
}

export function getHistory(params = {}, options = {}) {
  const query = params instanceof URLSearchParams
    ? new URLSearchParams(params)
    : new URLSearchParams(Object.entries(params).filter(([, value]) => value !== undefined && value !== null));
  const suffix = query.toString();
  return request(suffix ? `/api/history?${suffix}` : "/api/history", options);
}

export function post(path, body, options = {}) {
  if (typeof path !== "string" || !path.startsWith("/api/")) {
    return Promise.reject(new ApiError("invalid_path", "Invalid New Dash API path."));
  }
  const token = document.querySelector('meta[name="new-dash-control-token"]')?.content;
  if (!token) {
    return Promise.reject(new ApiError("missing_token", "The control token is unavailable."));
  }
  return request(path, {
    ...options,
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-New-Dash-Token": token,
    },
    body: JSON.stringify(body),
  });
}

export function createCompletionPoller({
  load,
  onValue,
  onError,
  intervalMs,
  schedule = (callback, delay) => window.setTimeout(callback, delay),
  cancel = (timer) => window.clearTimeout(timer),
}) {
  let stopped = true;
  let generation = 0;
  let timer = null;
  let activeController = null;

  async function run(runGeneration) {
    if (stopped || runGeneration !== generation) {
      return;
    }
    const controller = new AbortController();
    activeController = controller;
    try {
      const value = await load(controller.signal);
      if (!stopped && runGeneration === generation) {
        onValue(value);
      }
    } catch (error) {
      if (!controller.signal.aborted && !stopped && runGeneration === generation) {
        onError(error);
      }
    } finally {
      if (runGeneration !== generation) {
        return;
      }
      activeController = null;
      if (!stopped) {
        timer = schedule(() => {
          timer = null;
          void run(runGeneration);
        }, intervalMs);
      }
    }
  }

  return {
    start() {
      if (!stopped) {
        return;
      }
      stopped = false;
      generation += 1;
      void run(generation);
    },
    stop() {
      if (stopped) {
        return;
      }
      stopped = true;
      generation += 1;
      if (timer !== null) {
        cancel(timer);
        timer = null;
      }
      activeController?.abort();
      activeController = null;
    },
  };
}
