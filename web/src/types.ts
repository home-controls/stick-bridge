// Shapes of the bridge's JSON API (main/web.c).

export interface AuthState {
  setup_required: boolean;
  authed: boolean;
  locked: boolean;
  api_token_set: boolean;
  api: string;
  version: string;
  hostname: string;
}

export interface Ash {
  bytes: number; frames: number; data: number; ack: number; nak: number; rst: number; rstack: number;
  error: number; retx: number; crc_err: number; cancel: number; substitute: number; oversize: number;
}

export interface UpdateState {
  state: "idle" | "checking" | "uptodate" | "available" | "downloading" | "rebooting" | "error";
  current: string; latest: string; notes: string; date: string; size: number; done: number;
  md5: string; sha256: string; checked_ago: number; error: string; source: string; auto: boolean;
  /** A firmware that was installed but didn't start, so the bridge went back ('' if none). */
  rolled_back?: string;
}

export interface Status {
  update_available?: string;
  fw: { version: string; project: string; idf: string; built: string; partition: string; pending_verify: boolean };
  sys: { chip: string; reset_reason: string; watchdog?: string; temp_c: number; heap_free: number; heap_min: number; heap_largest: number;
    psram_total?: number; psram_free?: number; psram_min?: number };
  uptime: number;
  epoch: number;
  stats_since: number;
  health: { score: number; reason: string };
  eth: { link: boolean; has_ip: boolean; speed: number; full_duplex: boolean; ip: string; mask: string; gw: string; mac: string; mode: "static" | "dhcp"; hostname: string };
  dongle: {
    present: boolean; open: boolean; usb_id: string; manufacturer: string; product: string; serial: string; since: number; baud: number;
    // 1.0.19+: what the bridge recognised
    usb_chip?: string; model?: string; model_name?: string; radio?: string; family?: string; protocol?: string; protocol_detected?: boolean;
    z2m_adapter?: string; model_baud?: number; rtscts_wired?: boolean; bootloader_entry?: string; fw_board?: string; flashable?: boolean; note?: string;
    model_downloaded?: boolean; fw_running?: string;  // 1.0.20+
  };
  client: { connected: boolean; ip: string; port: number; since: number; allowed: string; tcp_port: number };
  ncp: { ezsp: number; stack: string; reset_code: number; error_code: number; network: "up" | "down" | "unknown" };
  counters: {
    host: Ash; ncp: Ash;
    ezsp_cmds: number; ezsp_responses: number; ezsp_callbacks: number;
    zb_rx: number; zb_unicast: number; zb_broadcast: number; zb_multicast: number; zb_sent_ok: number; zb_sent_fail: number;
    route_errors: number; net_status: number; joins: number; leaves: number; id_conflicts: number;
    rtt_avg_ms: number; rtt_max_ms: number; rtt_n: number; rtt_hist: number[];
    ack_avg_ms: number; ack_max_ms: number;
    clients_accepted: number; clients_refused: number; client_drops: number;
    dropped_no_client: number; dropped_overflow: number; dongle_tx_errors: number;
    dongle_attach: number; dongle_detach: number;
    flow?: Flow;  // firmware 1.0.17+
    line?: Line;  // firmware 1.0.18+
  };
  radios?: Radio[];  // 1.0.23+: the shown radios (2.0: radio 0 always, others while they have a stick or are set up)
}

export type Role = "zigbee" | "zha" | "thread" | "zwave";  // zha: not on radio 0

/** no_stick: radio 0 without a stick; missing: set up, its stick unplugged; opening; ready; flashing. */
export type RadioState = "no_stick" | "missing" | "opening" | "ready" | "flashing";

/** One shown radio (2.0): its stick on J3 or behind a hub, its TCP port and client. */
export interface Radio {
  index: number; state: RadioState; set_up: boolean; error: string; gen: number;
  name: string; role: Role; tcp_port: number; allowed: string; baud: number;
  stick: {
    present: boolean; open: boolean; since: number;
    usb_id?: string; manufacturer?: string; product?: string; serial?: string; usb_chip?: string; model?: string; model_name?: string;
    family?: string; protocol?: string; protocol_detected?: boolean; z2m_adapter?: string; fw_running?: string; hub_port?: number;
  };
  /** The stick this radio keeps (the first one it saw); null = the next new stick takes it. */
  saved_stick: { usb_id: string; serial: string; hub_port: number } | null;
  client: { connected: boolean; ip: string; port: number; since: number };
  counters: {
    bytes_in: number; bytes_out: number; bps_in: number; bps_out: number;
    clients_accepted: number; clients_refused: number; client_drops: number;
    dropped_no_client: number; dropped_overflow: number; tx_errors: number; attach: number; detach: number;
  };
}

/** GET /api/usb (1.0.23+): what is on J3. */
export interface UsbState {
  fs_only: boolean; fs_forced: boolean; reforces: number; refused: number; radio_max: number;
  devices: { addr: number; hub_port: number; hub: boolean; speed: "high" | "full"; usb_id: string; product: string; radio: number | null }[];
}

/** A radio's link in /api/config (2.0: all four); radio 0's mirror the flat fields. `gen` goes back with a change. */
export interface RadioLink { index: number; shown: boolean; set_up: boolean; gen: number; name: string; tcp_port: number; allow: string; baud: number; role: Role }

/** USB OUT pacing + XON/XOFF from the stick (txpace_core.c). */
export interface Flow {
  xon_rx: number; xoff_rx: number; xoff_now: boolean;
  xoff_pauses: number; xoff_timeouts: number; xoff_pause_avg_ms: number; xoff_pause_max_ms: number;
  writes: number; chunks: number; paced: number; pace_avg_us: number; pace_max_us: number;
  write_max_ms: number; write_errors: number; queue_hwm: number;
  chunk: number; gap_us: number; xoff_honor: boolean; xoff_max_ms: number;
}

/** The CP2102N's own view of the UART: CTS from the stick, its error flags and queues (1.0.18+). */
export interface Line {
  known: boolean; cts_now: boolean; hw_flow: boolean;
  samples: number; cts_off: number; cts_drops: number;
  status_polls: number; hold_cts: number; hold_xon: number;
  err_framing: number; err_hw_overrun: number; err_queue_overrun: number; err_parity: number; err_break: number;
  in_queue_max: number; out_queue_max: number; poll_fail: number; hw_auto_off: number;
}

export interface History {
  t: number[]; in: number[]; out: number[]; rx: number[]; ok: number[]; fail: number[];
  cmds: number[]; rtt: number[]; rttmax: number[]; err: number[];
  /** 2.0: bytes/s from / to each radio's stick (all four, aligned with t; radio 0's equal in/out). */
  radios?: { in: number[]; out: number[] }[];
}

/** radio: 0-3 = the radio it is about, -1 = the bridge as a whole (2.0; missing before). */
export interface BridgeEvent { id: number; up: number; epoch: number; level: 0 | 1 | 2; text: string; radio?: number }

export interface ZbDevice {
  short: string; ieee: string; rx: number; tx_ok: number; tx_fail: number; route_err: number;
  lqi: number; rssi: number; lqi_avg: number; rssi_avg: number; first: number; last: number;
  last_fail: number; last_fail_status: number;
}

export interface Config {
  hostname: string; mode: "static" | "dhcp"; ip: string; mask: string; gw: string; dns: string;
  allow: string; tcp_port: number; baud: number; update_url: string; update_auto: boolean;
  tx_chunk?: number; tx_gap_us?: number; xoff_honor?: boolean; xoff_max_ms?: number;  // 1.0.17+
  hw_flow?: boolean;  // 1.0.18+
  fw_repo?: string; fw_prerelease?: boolean;  // 1.0.19+
  role?: Role;  // 1.0.20+ (zwave 1.0.23+)
  usb_fs_only?: boolean;  // 1.0.23+
  radios?: RadioLink[];  // 1.0.23+
}

/** GET /api/dongle -> catalog (2.0: per radio): builds for that radio's stick on GitHub. */
export interface FwCatalog {
  radio: number; checking: boolean; checked: number; checked_ago: number; error: string; repo: string; release: string; prerelease: boolean;
  board: string; model: string; current: string; type?: string;
  dongle_list?: { status: string; count: number; fetched_ago: number };
  builds: { name: string; version: string; sdk: string; baud: number; flow: string; size: number; sha256: string;
    usable: boolean; recommended: boolean; vs_current: "newer" | "same" | "older" | "unknown" }[];
  download: { state: "idle" | "running" | "done" | "failed"; radio?: number; name: string; done: number; total: number; error: string };
}

/** GET /api/dongles: the sticks the bridge recognises. */
export interface DongleModel {
  id: string; name: string; usb_id: string; usb_chip: string; radio: string; family: string; protocol: string; baud: number;
  rtscts_wired: boolean; bootloader_entry: string; flashable: boolean; note: string; downloaded?: boolean;
}

/** GET /api/dongle: the picked radio's stick firmware (an image stored on the bridge, flashed on request). */
export interface DongleFw {
  catalog?: FwCatalog;
  max_size: number; baud: number; busy: boolean; dongle: boolean; z2m_connected: boolean;
  radio?: number;  // 1.0.23+: the radio whose stick these pages work on
  current: string | null;
  image: null | {
    name: string; size: number; upload_size: number; md5: string; sha256: string; uploaded?: number;
    signed: boolean; encrypted: boolean; fw_type?: string; version?: string; sdk?: string; image_baud?: number;
    problem?: string; flow?: string; sets_baud?: number; sets_hw_flow?: boolean;  // 1.0.19+
    kind?: string; summary?: string;  // 1.0.20+
  };
  flash: {
    state: "idle" | "running" | "done" | "failed";
    step?: "image" | "bootloader" | "upload" | "start" | "verify"; done?: number; total?: number;
    error?: string; started?: number; seconds?: number; entered?: string; bootloader?: string;
    before?: string; after?: string; dongle_left?: "running" | "bootloader" | "unknown"; retries?: number;
  };
}
/** GET /api/logs: the stored log in flash and its copy on a microSD card. Times are Unix ms. */
export interface LogsInfo {
  flash: {
    ok: boolean; where: string; chip_id: string; chip_size: number; boot: number; clock_wait: boolean;
    offset?: number; size?: number; used?: number; oldest?: number; write_errors?: number; pending?: number;
  };
  sd: {
    state: "ok" | "none" | "error" | "busy"; detail: string; retry_in?: number;
    total?: number; free?: number; card?: string; khz?: number; written?: number; mounted_s?: number;
    file_count?: number; file_bytes?: number; files?: { name: string; size: number }[];
  };
}
