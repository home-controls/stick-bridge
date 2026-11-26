// State as signals, fed by polling the bridge's JSON API. Pages read the signals; Preact
// re-renders whatever used a signal when it changes.

import { computed, signal } from "@preact/signals";
import type { AuthState, BridgeEvent, Config, History, Status, ZbDevice } from "./types";
import { route } from "./route";

export const auth = signal<AuthState | null>(null);
export const status = signal<Status | null>(null);
export const history = signal<History | null>(null);
/** The homepage charts' time range; 15 min uses the per-second history, longer ones the 10-second one. */
export type ChartRange = "15m" | "1h" | "6h" | "24h";
export const RANGES: Record<ChartRange, { secs: number; bucket: number; label: string }> = {
  "15m": { secs: 900, bucket: 10, label: "15 min" },
  "1h": { secs: 3600, bucket: 30, label: "1 h" },
  "6h": { secs: 21600, bucket: 120, label: "6 h" },
  "24h": { secs: 86400, bucket: 600, label: "24 h" },
};
function savedRange(): ChartRange {
  try { const v = localStorage.getItem("chartRange"); if (v && v in RANGES) return v as ChartRange; } catch { /* private mode */ }
  return "15m";
}
export const chartRange = signal<ChartRange>(savedRange());
export function setChartRange(r: ChartRange) {
  chartRange.value = r;
  try { localStorage.setItem("chartRange", r); } catch { /* private mode */ }
  if (r !== "15m") pollHistoryLong().catch(() => undefined);
}
/** 10-second points (firmware 1.0.21+), null until fetched or when the bridge doesn't have them. */
export const historyLong = signal<History | null>(null);
export const events = signal<BridgeEvent[]>([]);
export const devices = signal<ZbDevice[]>([]);
export const devicesUptime = signal(0);
export const config = signal<Config | null>(null);
export const reachable = signal(true);
export const rebootNeeded = signal(false);
export const logText = signal("");

/** Browser clock time (s) of the bridge's boot, so uptime stamps become wall-clock times. */
export const bootEpoch = signal(0);
export const nowS = () => Date.now() / 1000;
export const atUptime = (up: number) => bootEpoch.value + up;

import { toast } from "./kit/toast";

export { toast };
export { toasts, type Toast } from "./kit/toast";

export class ApiError extends Error {
  constructor(message: string, public status: number) { super(message); }
}

export async function api<T = any>(path: string, opts: { method?: string; body?: unknown; raw?: BodyInit } = {}): Promise<T> {
  const method = opts.method || (opts.body !== undefined || opts.raw !== undefined ? "POST" : "GET");
  const headers: Record<string, string> = {};
  if (method !== "GET") headers["X-Bridge"] = "1";
  if (opts.body !== undefined) headers["Content-Type"] = "application/json";
  let r: Response;
  try {
    r = await fetch(path, { method, headers, credentials: "same-origin", cache: "no-store",
      body: opts.raw ?? (opts.body !== undefined ? JSON.stringify(opts.body) : undefined) });
  } catch {
    reachable.value = false;
    throw new ApiError("The bridge isn't answering", 0);
  }
  reachable.value = true;
  let j: any = null;
  try { j = await r.json(); } catch { /* not JSON */ }
  if (r.status === 401 || (r.status === 403 && j?.error === "setup required")) {
    void loadAuth();
    throw new ApiError(j?.error || "login required", r.status);
  }
  if (!r.ok) throw new ApiError(j?.error || `HTTP ${r.status}`, r.status);
  return j as T;
}

export const post = <T = any>(path: string, body: unknown = {}) => api<T>(path, { method: "POST", body });

export async function loadAuth() {
  try {
    const r = await fetch("/api/state", { credentials: "same-origin", cache: "no-store" });
    auth.value = await r.json();
    reachable.value = true;
  } catch {
    reachable.value = false;
  }
}

// ------------------------------------------------------------------ polling

let evSince = 0;
let logNext = 0;
const quiet = () => document.visibilityState === "hidden";

async function pollStatus() {
  const s = await api<Status>("/api/status");
  bootEpoch.value = (s.epoch || nowS()) - s.uptime;
  status.value = s;
}
async function pollHistory() { history.value = await api<History>("/api/history"); }
async function pollHistoryLong() {
  const n = Math.ceil(RANGES[chartRange.value].secs / 10) + 1;
  historyLong.value = await api<History>(`/api/history?res=10&n=${n}`);
}
async function pollEvents() {
  const r = await api<{ events: BridgeEvent[] }>(`/api/events?since=${evSince}`);
  // Two polls can overlap (first load + timer): keep only ids not seen yet.
  const fresh = r.events.filter((e) => e.id > evSince);
  if (fresh.length) {
    evSince = fresh[fresh.length - 1].id;
    events.value = [...events.value, ...fresh].slice(-400);
  }
}
async function pollDevices() {
  const r = await api<{ uptime: number; devices: ZbDevice[] }>("/api/devices");
  devicesUptime.value = r.uptime;
  devices.value = r.devices;
}
export async function pollLog(reset = false) {
  if (reset) { logNext = 0; logText.value = ""; }
  const r = await api<{ next: number; text: string }>(`/api/log?since=${logNext}`);
  if (logNext && r.next < logNext) logText.value = "";  // bridge rebooted
  logNext = r.next;
  if (r.text) logText.value = (logText.value + r.text).slice(-120000);
}
export async function loadConfig() { config.value = await api<Config>("/api/config"); }

export function resetEvents() { evSince = 0; events.value = []; }

let started = false;
export function startPolling() {
  if (started) return;
  started = true;
  const every = (ms: number, fn: () => Promise<void> | void, when: () => boolean = () => true) => {
    const run = () => { if (auth.value?.authed && !quiet() && when()) Promise.resolve(fn()).catch(() => undefined); };
    run();
    setInterval(run, ms);
  };
  every(2000, pollStatus);
  every(4000, pollEvents);
  every(5000, pollHistory, () => route.value[0] === "overview");
  every(15000, pollHistoryLong, () => route.value[0] === "overview" && chartRange.value !== "15m");
  every(4000, pollDevices, () => route.value[0] === "devices" || route.value[0] === "overview");
  every(2000, () => pollLog(), () => route.value[0] === "system");
  document.addEventListener("visibilitychange", () => {
    if (!quiet() && auth.value?.authed) { pollStatus().catch(() => undefined); pollEvents().catch(() => undefined); }
  });
}

export async function refreshAll() {
  await Promise.allSettled([pollStatus(), pollHistory(), pollEvents(), pollDevices(), loadConfig()]);
}

// ------------------------------------------------------------------ derived

/** Sum of the last n seconds of a history column. */
export function lastN(col: keyof History, n: number): number {
  const h = history.value;
  if (!h) return 0;
  const a = h[col];
  let s = 0;
  for (let i = Math.max(0, a.length - n); i < a.length; i++) s += a[i];
  return s;
}

export const delivery = computed(() => {
  const ok = lastN("ok", 300), fail = lastN("fail", 300);
  return { ok, fail, pct: ok + fail ? (ok / (ok + fail)) * 100 : null };
});

export const rttLastMinute = computed(() => {
  const h = history.value;
  if (!h) return { avg: null as number | null, max: 0 };
  let s = 0, n = 0, max = 0;
  for (let i = Math.max(0, h.t.length - 60); i < h.t.length; i++) {
    if (h.rtt[i]) { s += h.rtt[i]; n++; }
    max = Math.max(max, h.rttmax[i]);
  }
  return { avg: n ? s / n : null, max };
});

/** Wait for the bridge to come back after a reboot, then reload the page. */
export function awaitReboot(note: string) {
  toast(note, "info", 60000);
  const t0 = Date.now();
  const poll = async () => {
    try {
      const r = await fetch("/api/state", { cache: "no-store" });
      if (r.ok && Date.now() - t0 > 4000) { location.reload(); return; }
    } catch { /* still down */ }
    setTimeout(poll, 1500);
  };
  setTimeout(poll, 3000);
}
