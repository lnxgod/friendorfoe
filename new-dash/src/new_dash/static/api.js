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
  const timeout = window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
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
    if (error?.name === "AbortError") {
      throw new ApiError("request_timeout", "New Dash did not respond within five seconds.");
    }
    throw new ApiError("network_error", "New Dash could not be reached.");
  } finally {
    window.clearTimeout(timeout);
  }
}

export function getState() {
  return request("/api/state");
}

export function getHistory(params = {}) {
  const query = params instanceof URLSearchParams
    ? new URLSearchParams(params)
    : new URLSearchParams(Object.entries(params).filter(([, value]) => value !== undefined && value !== null));
  const suffix = query.toString();
  return request(suffix ? `/api/history?${suffix}` : "/api/history");
}

export function post(path, body) {
  if (typeof path !== "string" || !path.startsWith("/api/")) {
    return Promise.reject(new ApiError("invalid_path", "Invalid New Dash API path."));
  }
  const token = document.querySelector('meta[name="new-dash-control-token"]')?.content;
  if (!token) {
    return Promise.reject(new ApiError("missing_token", "The control token is unavailable."));
  }
  return request(path, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-New-Dash-Token": token,
    },
    body: JSON.stringify(body),
  });
}
