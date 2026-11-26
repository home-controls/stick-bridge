// Several radios on one bridge (2.0: up to four, added by plugging a stick into the hub): the radios list on the
// Overview, the USB devices card on System and the Radios settings. A bridge with one stick shows little of it.

import { useEffect, useRef, useState } from "preact/hooks";
import { ROLE_CLIENT, config, extraRadios, loadConfig, pollUsb, post, radioAt, radioPick, rebootNeeded, setRadioPick, status, toast, usb } from "../store";
import { bytes, duration, num } from "../format";
import { Card, Chip, Empty, Field, KV, Seg, copyText } from "../ui";
import { Icon } from "../icons";
import { go } from "../route";
import { confirmIt } from "../dialog";
import type { Config, Radio, RadioLink, Role } from "../types";

export const ROLE_LABEL: Record<Role, string> = { zigbee: "Zigbee", zha: "ZHA", thread: "Thread", zwave: "Z-Wave" };
const ROLE_LONG: Record<Role, string> = {
  zigbee: "Zigbee coordinator (Zigbee2MQTT)",
  zha: "Zigbee coordinator (Home Assistant ZHA)",
  thread: "Thread RCP (OpenThread Border Router)",
  zwave: "Z-Wave controller (Z-Wave JS)",
};
// Zigbee2MQTT adapter (from the stick's protocol) -> ZHA's radio type choice. ZHA has no ZBOSS (nRF) support.
const ZHA_RADIO: Record<string, string> = {
  ember: "EZSP (Silicon Labs EmberZNet)",
  ezsp: "EZSP (Silicon Labs EmberZNet)",
  zstack: "ZNP (Texas Instruments Z-Stack)",
  deconz: "deCONZ (ConBee / RaspBee)",
  zigate: "ZiGate",
  zboss: "none: ZHA can't drive ZBOSS (nRF) sticks; use Zigbee2MQTT",
};
export const where = (port?: number) => (port ? `hub port ${port}` : "J3");

export function stickName(r: Radio) {
  const k = r.stick;
  return k.model_name && !/^Unknown/.test(k.model_name) ? k.model_name : k.product || k.model_name || "USB serial";
}

/** Who uses a radio's stick, from its role: "Zigbee2MQTT", "the border router". */
export const clientOf = (role: Role) => ROLE_CLIENT[role] ?? "its client";
export const cap = (t: string) => t.charAt(0).toUpperCase() + t.slice(1);

/** A radio's state as a chip. Missing sticks and errors are red. */
export function radioState(r: Radio): { tone: "ok" | "warn" | "bad" | "muted"; text: string } {
  if (r.state === "missing") return { tone: "bad", text: "Stick missing" };
  if (r.state === "no_stick") return { tone: "bad", text: "No stick" };
  if (r.error) return { tone: "bad", text: "Error" };
  if (r.state === "flashing") return { tone: "warn", text: "Being flashed" };
  if (r.state === "opening") return { tone: "warn", text: "Opening" };
  if (!r.client.connected) return { tone: "warn", text: "No client" };
  return { tone: "ok", text: "Connected" };
}

/** The red line under a radio for a missing stick or an error, else null. */
export function radioProblem(r: Radio): string | null {
  if (r.state === "missing") return `Stick missing: ${clientOf(r.role)} can't reach its network until it's plugged back in`;
  return r.error || null;
}

/** The client configuration for a radio's role. */
export function radioSnippet(role: Role, ip: string, port: number, baud: number, adapter?: string) {
  if (role === "thread")
    return `# Home Assistant: OpenThread Border Router add-on -> Configuration\n# device: leave any existing serial port selected (the add-on requires one)\nnetwork_device: ${ip}:${port}\nbaudrate: "${baud}"\nflow_control: false`;
  if (role === "zwave")
    return `# Z-Wave JS UI -> Settings -> Z-Wave -> Serial Port (type it in)\ntcp://${ip}:${port}`;
  if (role === "zha") {
    const type = ZHA_RADIO[adapter || ""];
    return `# Home Assistant: Settings -> Devices & services -> Add integration -> Zigbee Home Automation\n# Serial port: Enter manually\nRadio type: ${type || "the one for your stick (shown here once it has talked)"}\nSerial device path: socket://${ip}:${port}\nPort speed: ${baud}\nData flow control: none\n# This forms a second Zigbee network: keep it on a different channel from Zigbee2MQTT's\n# (ZHA's configuration page can change it)`;
  }
  return `serial:\n  port: tcp://${ip}:${port}\n  adapter: ${adapter || "ember"}\n  baudrate: ${baud}\n  rtscts: false`;
}

/** Overview: every shown radio on its own row; a row opens that radio's own view. */
export function RadiosRow() {
  const s = status.value;
  if (!s?.radios || extraRadios.value.length === 0) return null;
  const rows = s.radios;
  return (
    <div style={{ marginTop: "16px" }}>
    <Card title={<><Icon name="usb" size={18} /> Radios</>} actions={<span class="muted small">one TCP port each · pick one for its own view</span>}>
      <div class="table-wrap"><table class="data radio-rows">
        <thead><tr><th>Radio</th><th>Stick</th><th class="num">TCP</th><th>Client</th><th class="num">From / to stick</th><th>State</th></tr></thead>
        <tbody>
          {rows.map((r) => {
            const st = radioState(r);
            const bad = radioProblem(r);
            const open = () => { setRadioPick(r.index); go("overview"); };
            return (
              <tr class="clickable" tabIndex={0} title={`Radio ${r.index + 1}'s own view`} onClick={open}
                onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); open(); } }}>
                <td><b>{r.index + 1}. {r.name}</b><div class="muted small">{ROLE_LABEL[r.role]}</div></td>
                <td>{r.stick.present
                  ? <>{stickName(r)}<div class="muted small">{where(r.stick.hub_port)}{r.stick.protocol && r.stick.protocol !== "unknown" ? ` · ${r.stick.protocol}` : ""}</div></>
                  : <span class="muted">{r.saved_stick ? `waiting for ${r.saved_stick.usb_id}${r.saved_stick.serial ? ` (${r.saved_stick.serial})` : ""}` : "—"}</span>}
                  {bad && <div class="small bad-text">{bad}</div>}</td>
                <td class="num mono">{r.tcp_port}</td>
                <td>{r.client.connected ? <>{r.client.ip}<div class="muted small">{duration(s.uptime - r.client.since)}</div></> : <span class="muted">—</span>}</td>
                <td class="num">{bytes(r.counters.bps_in)}/s · {bytes(r.counters.bps_out)}/s</td>
                <td><Chip tone={st.tone}>{st.text}</Chip></td>
              </tr>
            );
          })}
        </tbody>
      </table></div>
    </Card>
    </div>
  );
}

/** The radio picker the Overview, Devices and Events pages share ("All radios" and each shown radio; the pick is
 *  remembered in one place). Nothing with only radio 1 shown. */
export function RadioSwitcher() {
  if (extraRadios.value.length === 0) return null;
  const rs = status.value?.radios ?? [];
  const v = radioPick.value === null ? "all" : String(radioPick.value);
  return (
    <div class="radio-switch">
      <Seg label="Radio" value={v} onChange={(x) => setRadioPick(x === "all" ? null : Number(x))}
        options={[{ v: "all", label: "All radios" }, ...rs.map((r) => ({ v: String(r.index),
          label: <>{radioProblem(r) ? <span class="dot bad" title={radioProblem(r) ?? ""} /> : null}{r.index + 1}. {r.name}</> }))]} />
    </div>
  );
}

/** One radio's own facts and counters: state, stick, client, traffic. */
export function RadioStatsCard({ r, title }: { r: Radio; title?: string }) {
  const s = status.value;
  if (!s) return null;
  const st = radioState(r);
  const bad = radioProblem(r);
  const k = r.counters;
  const warn = (v: number) => (v ? <span class="warn-text">{num(v)}</span> : num(v));
  return (
    <Card title={<><Icon name="usb" size={18} /> {title ?? `Radio ${r.index + 1} · ${r.name}`}</>} actions={<Chip tone={st.tone}>{st.text}</Chip>}>
      {bad && <p class="small bad-text" style={{ marginTop: 0 }}><Icon name="alert" size={14} /> {bad}</p>}
      <KV rows={[
        ["Role", ROLE_LONG[r.role]],
        ["Stick", r.stick.present ? <>{stickName(r)} <span class="muted">on {where(r.stick.hub_port)}</span></>
          : r.saved_stick ? <span class="muted">waiting for {r.saved_stick.usb_id}{r.saved_stick.serial ? ` (${r.saved_stick.serial})` : ""}</span> : <span class="muted">none</span>],
        ...(r.stick.present ? [
          ["USB", <span class="mono">{r.stick.usb_id ?? "—"}{r.stick.usb_chip ? ` · ${r.stick.usb_chip}` : ""} · {num(r.baud)} baud</span>] as [string, any],
          ["Protocol", r.stick.protocol && r.stick.protocol !== "unknown" ? <>{r.stick.protocol}{!r.stick.protocol_detected ? <span class="muted"> (expected)</span> : null}</> : <span class="muted">not seen yet</span>] as [string, any],
          ["Firmware", r.stick.fw_running || (r.index === 0 && s.dongle.fw_running) || <span class="muted">shows once its client has talked</span>] as [string, any],
          ["Stick for", duration(s.uptime - r.stick.since)] as [string, any],
        ] : []),
        ["Client", r.client.connected ? `${r.client.ip}:${r.client.port} · ${duration(s.uptime - r.client.since)}` : <span class="muted">waiting on tcp/{r.tcp_port}</span>],
        ["Allowed host", r.allowed || "anyone"],
        ["Now", `${bytes(k.bps_in)}/s from the stick · ${bytes(k.bps_out)}/s to it`],
        ["Moved", `${bytes(k.bytes_in)} from · ${bytes(k.bytes_out)} to the stick`],
        ["Sessions", <>{num(k.clients_accepted)} accepted · {warn(k.clients_refused)} refused · {warn(k.client_drops)} dropped</>],
        ["Bytes dropped", <>{warn(k.dropped_no_client)} with no client · {warn(k.dropped_overflow)} overflow</>],
        ["Write errors", warn(k.tx_errors)],
        ["Plugged in / out", `${num(k.attach)} / ${num(k.detach)}`],
      ]} />
    </Card>
  );
}

/** System: what is plugged into J3, at what speed, and which radio has which stick. */
export function UsbCard() {
  useEffect(() => { void pollUsb().catch(() => undefined); }, []);
  const u = usb.value;
  if (!u) return null;  // firmware before 1.0.23
  const hub = u.devices.find((d) => d.hub);
  const list = [...u.devices].sort((a, b) => Number(b.hub) - Number(a.hub) || a.hub_port - b.hub_port);
  return (
    <Card title={<><Icon name="usb" size={18} /> USB devices</>} actions={<span class="muted small">{u.fs_only ? "J3 full speed only" : "J3 full or high speed"}</span>}>
      {list.length === 0 ? <Empty>Nothing plugged into J3</Empty> : (
        <div class="table-wrap"><table class="data">
          <thead><tr><th>Device</th><th>Where</th><th>Speed</th><th>Radio</th></tr></thead>
          <tbody>
            {list.map((d) => (
              <tr>
                <td>{d.hub ? "USB hub" : d.product || "USB device"}<div class="muted small mono">{d.usb_id}</div></td>
                <td>{d.hub ? "J3" : where(d.hub_port)}</td>
                <td>{d.speed === "high" ? <span class={d.hub ? "warn-text" : ""}>high</span> : "full"}</td>
                <td>{d.radio !== null ? `${d.radio + 1}. ${radioAt(d.radio)?.name ?? ""}` : d.hub ? "" : <span class="muted">not used</span>}</td>
              </tr>
            ))}
          </tbody>
        </table></div>
      )}
      {hub?.speed === "high" && (
        <p class="small warn-text"><Icon name="alert" size={14} /> The hub runs at high speed, so the sticks behind it can't work (the P4 has no transaction translator).
          {u.fs_only ? " The bridge is forcing J3 back to full speed." : " Turn on full-speed-only USB in Settings → Radios, or use a USB 1.1 hub."}</p>
      )}
      {u.refused > 0 && <p class="small muted">{u.refused} stick{u.refused === 1 ? "" : "s"} found no free radio (see Events).</p>}
      <p class="field-help">Up to {u.radio_max} radio sticks behind one powered hub. A new stick takes the lowest free radio; a set-up radio keeps its stick (by serial number, else by hub port).</p>
    </Card>
  );
}

export function Msg({ m }: { m: { ok: boolean; text: string } | null }) {
  return m ? <span class={`form-msg ${m.ok ? "ok" : "bad"}`}>{m.text}</span> : null;
}

/** The baud rates the dashboard offers. */
export const BAUDS = [115200, 230400, 460800, 921600];

/** The address clients connect to: the static IP, else what the bridge reports, else this page's host. */
export function bridgeIp(c: Config) { return c.mode === "static" ? c.ip : status.value?.eth.ip || location.hostname; }

/** A config snippet with a copy button. */
export function Snippet({ text, class: cls }: { text: string; class?: string }) {
  return (
    <pre class={`snippet${cls ? ` ${cls}` : ""}`}>{text}
      <button type="button" class="icon-btn" title="Copy" aria-label="Copy" onClick={async () => toast((await copyText(text)) ? "Copied" : "Copy failed", "ok")}><Icon name="copy" size={16} /></button>
    </pre>
  );
}

const CLIENT_HEAD: Record<Role, string> = {
  zigbee: "Zigbee2MQTT configuration",
  zha: "ZHA setup",
  thread: "Border router configuration",
  zwave: "Z-Wave JS configuration",
};

// The stick's detected protocol against the radio's role: what to do about it, or null when they fit
// (or nothing has been detected yet).
function roleMismatch(role: Role, r?: Radio): string | null {
  if (!r?.stick.present || !r.stick.protocol_detected) return null;
  const p = r.stick.protocol || "";
  const kind = /Spinel/.test(p) ? "thread" : /Z-Wave/.test(p) ? "zwave" : /EmberZNet|Z-Stack|deCONZ|ZiGate|ZBOSS/.test(p) ? "zigbee" : null;
  const want = role === "zha" ? "zigbee" : role;
  if (!kind || kind === want) return null;
  if (role === "zwave") return `The stick talks ${p}, not Z-Wave: pick the role that matches it.`;
  if (kind === "zwave") return `The stick is a Z-Wave controller (${p}): set the role to Z-Wave.`;
  return `The stick talks ${p}: flash ${role === "thread" ? "an OpenThread RCP" : "a Zigbee coordinator"} build for this role (System → Stick firmware), or change the role.`;
}

const EDITABLE = ["name", "role", "tcp_port", "allow", "baud"] as const;
const differs = (a: RadioLink, b: RadioLink) => EDITABLE.some((k) => String(a[k]).trim() !== String(b[k]).trim());
const copyLinks = (c: Config) => (c.radios ?? []).map((r) => ({ ...r }));

/** Settings: one panel per shown radio, all in the same layout, and the full-speed-only port. */
export function RadiosSettingsCard() {
  const c = config.value;
  // d = what the form shows, base = the config each radio was loaded from (its gen goes back with a change)
  const [d, setD] = useState<RadioLink[] | null>(null);
  const [base, setBase] = useState<RadioLink[] | null>(null);
  const [fs, setFs] = useState<boolean>(true);
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);

  // A new config (a stick plugged in or out, a save): take it for every radio the user hasn't edited.
  useEffect(() => {
    if (!c?.radios) return;
    const fresh = copyLinks(c);
    if (!d || !base) { setD(fresh); setBase(fresh.map((r) => ({ ...r }))); setFs(c.usb_fs_only !== false); return; }
    const keep = (i: number) => { const x = d.find((r) => r.index === i), b = base.find((r) => r.index === i); return x && b && differs(x, b) ? { x, b } : null; };
    setD(fresh.map((r) => keep(r.index)?.x ?? r));
    setBase(fresh.map((r) => keep(r.index)?.b ?? { ...r }));
  }, [c]);

  // The shown radios or their generations moved on (status polls) while config didn't: load it again.
  const live = status.value?.radios ?? [];
  const sigLive = live.map((r) => `${r.index}:${r.gen}`).join(",");
  const sigCfg = (c?.radios ?? []).filter((r) => r.shown).map((r) => `${r.index}:${r.gen}`).join(",");
  const asked = useRef("");
  useEffect(() => {
    if (!c?.radios || !status.value?.radios || sigLive === sigCfg || asked.current === sigLive) return;
    asked.current = sigLive;
    void loadConfig().catch(() => undefined);
  }, [sigLive, sigCfg]);

  if (!c || !c.radios) return null;  // firmware before 1.0.23
  if (!d || !base) return <Card title="Radios" class="span-all"><p class="muted">Loading…</p></Card>;
  const ip = bridgeIp(c);
  const upd = (i: number, p: Partial<RadioLink>) => setD((x) => (x ? x.map((r) => (r.index === i ? { ...r, ...p } : r)) : x));
  const startOver = (cfg: Config) => { const f = copyLinks(cfg); setD(f); setBase(f.map((r) => ({ ...r }))); setFs(cfg.usb_fs_only !== false); };
  const shown = d.filter((r) => c.radios!.find((x) => x.index === r.index)?.shown);

  const submit = async (e: Event) => {
    e.preventDefault();
    setM(null);
    const changed = d.filter((r) => { const b = base.find((x) => x.index === r.index); return b && differs(r, b); });
    const fsChanged = fs !== (c.usb_fs_only !== false);
    if (!changed.length && !fsChanged) { setM({ ok: true, text: "Nothing to save" }); setTimeout(() => setM(null), 3000); return; }
    try {
      const body: Record<string, unknown> = {};
      if (fsChanged) body.usb_fs_only = fs;
      if (changed.length) body.radios = changed.map((r) => ({ index: r.index, gen: base.find((x) => x.index === r.index)!.gen, name: r.name.trim(),
        role: r.role, tcp_port: Number(r.tcp_port), allow: r.allow.trim(), baud: Number(r.baud) }));
      const r = await post<Config & { reboot_required: boolean }>("/api/config", body);
      config.value = r;
      startOver(r);
      if (r.reboot_required) rebootNeeded.value = true;
      setM({ ok: true, text: r.reboot_required ? "Saved; reboot for the USB speed" : "Saved and applied" });
      setTimeout(() => setM(null), 4000);
    } catch (x: any) {
      setM({ ok: false, text: x.message });
      if (x?.status === 409) {
        try { await loadConfig(); if (config.value) startOver(config.value); } catch { /* the message is shown */ }
      }
    }
  };

  const forget = async (i: number, role: Role) => {
    if (!(await confirmIt({ title: `Forget radio ${i + 1}?`, confirm: "Forget",
      body: `The radio disappears and its TCP port closes; ${clientOf(role)} stays cut off. The next stick plugged into the hub takes the lowest free radio and starts afresh. Use this when the stick is gone for good or replaced.` }))) return;
    try { await post("/api/radio/forget", { radio: i }); toast(`Radio ${i + 1} forgotten`); await loadConfig(); } catch (x: any) { toast(x.message, "error"); }
  };

  return (
    <Card title={<><Icon name="usb" size={18} /> Radios</>} class="span-all">
      <p class="muted small" style={{ marginTop: 0 }}>Each radio is one USB stick and one TCP port: the bridge passes bytes between them, so the
        app on the other end (Zigbee2MQTT, ZHA, an OpenThread Border Router, Z-Wave JS) works as if the stick were plugged into it.
        Plugging a stick into the powered hub on J3 adds a radio by itself: its role is guessed from the stick, with the stick's own baud rate,
        a free TCP port and radio 1's allowed host. A radio is set up once a client has connected to it. Unplugging a stick nothing was set up on
        removes its radio; a set-up radio waits for its stick. Radio 1 is always shown and is the one the Overview decodes in depth, so it serves
        Zigbee2MQTT, a border router or Z-Wave JS, not ZHA. Settings apply at once.</p>
      <form onSubmit={submit}>
        <div class={shown.length === 3 ? "cols-3" : "cols-2"} style={{ marginBottom: 0 }}>
          {shown.map((r) => {
            const lv = live.find((x) => x.index === r.index);
            const cfg = c.radios!.find((x) => x.index === r.index)!;
            const snip = radioSnippet(r.role, ip, Number(r.tcp_port), Number(r.baud), lv?.stick.z2m_adapter);
            const roles = (Object.keys(ROLE_LONG) as Role[]).filter((k) => r.index > 0 || k !== "zha");
            const warn = roleMismatch(r.role, lv);
            const flashing = lv?.state === "flashing";
            const bad = lv ? radioProblem(lv) : null;
            return (
              <div class={`radio-box${bad ? " radio-bad" : ""}`}>
                <h4 style={{ marginTop: 0, marginBottom: "6px" }}>Radio {r.index + 1}{lv?.stick.present ? <span class="muted"> · {stickName(lv)} on {where(lv.stick.hub_port)}</span> : null}</h4>
                {bad && <p class="small bad-text radio-alert"><Icon name="alert" size={14} /> {bad}
                  {lv?.state === "missing" && <button type="button" class="btn small-btn" onClick={() => forget(r.index, r.role)}>Forget</button>}</p>}
                {!bad && lv?.state === "opening" && <p class="small warn-text radio-alert">Opening…</p>}
                {!bad && flashing && <p class="small warn-text radio-alert">Being flashed: port and baud rate wait until it's done</p>}
                {!bad && lv?.state === "no_stick" && <p class="small bad-text radio-alert">No stick: plug one into J3 or the hub</p>}
                <div class="form-grid two">
                  <span class="wide"><Field label="Name" help="Shown on the dashboard and in events; empty = the role">
                    <input value={r.name} maxLength={15} placeholder={ROLE_LABEL[r.role]} onInput={(e) => upd(r.index, { name: (e.target as HTMLInputElement).value })} />
                  </Field></span>
                  <span class="wide"><Field label="Role" help="What the stick is for: which firmware System offers for it, and the client configuration below">
                    <select value={r.role} onChange={(e) => upd(r.index, { role: (e.target as HTMLSelectElement).value as Role })}>
                      {roles.map((k) => <option value={k}>{ROLE_LONG[k]}</option>)}
                    </select>
                  </Field></span>
                  {warn && <p class="wide small warn-text" style={{ margin: 0 }}><Icon name="alert" size={14} /> {warn}</p>}
                  <Field label="TCP port" help="Where the client connects"><input type="number" min={1} max={65535} value={r.tcp_port} disabled={flashing} onInput={(e) => upd(r.index, { tcp_port: Number((e.target as HTMLInputElement).value) })} /></Field>
                  <Field label="Baud rate" help="The stick's serial speed">
                    <select value={String(r.baud)} disabled={flashing} onChange={(e) => upd(r.index, { baud: Number((e.target as HTMLSelectElement).value) })}>
                      {(BAUDS.includes(Number(r.baud)) ? BAUDS : [...BAUDS, Number(r.baud)]).map((b) => <option value={b}>{b.toLocaleString()}</option>)}
                    </select>
                  </Field>
                  <span class="wide"><Field label="Allowed host" help="Only this IP (or a range like 192.168.1.0/24) may connect; empty = anyone. A connected client the new rule doesn't allow is dropped.">
                    <input value={r.allow} placeholder="192.168.1.10" onInput={(e) => upd(r.index, { allow: (e.target as HTMLInputElement).value })} />
                  </Field></span>
                </div>
                <p class="field-help">
                  {lv?.saved_stick
                    ? <>Keeps stick <span class="mono">{lv.saved_stick.usb_id}</span>{lv.saved_stick.serial ? <> serial <span class="mono">{lv.saved_stick.serial}</span></> : <> on {where(lv.saved_stick.hub_port)}</>}.{" "}</>
                    : r.index === 0 && !lv?.stick.present ? "No stick saved yet: the next stick plugged in takes this radio. " : null}
                  {cfg.set_up ? "Set up: it waits for its stick when unplugged." : "Not set up yet (no client so far): unplugging its stick removes this radio."}
                </p>
                <details class="chart-table">
                  <summary>{CLIENT_HEAD[r.role]}</summary>
                  <Snippet text={snip} />
                </details>
              </div>
            );
          })}
        </div>
        <div style={{ marginTop: "12px" }}>
          <Field label="Full-speed-only USB on J3" help="Makes a USB 2.0 or 3.0 hub run at full speed, which the radio sticks behind it need (the P4 can't reach full-speed sticks through a high-speed hub). A stick plugged straight in is full speed anyway. This one needs a reboot.">
            <Seg label="Full-speed-only USB" value={fs ? "on" : "off"} onChange={(v) => setFs(v === "on")} options={[{ v: "on", label: "On (for hubs)" }, { v: "off", label: "Off" }]} />
          </Field>
        </div>
        <div class="form-actions">
          <button class="btn primary">Save radios</button>
          <button type="button" class="btn" onClick={() => startOver(c)}>Undo changes</button>
          <Msg m={m} />
        </div>
      </form>
    </Card>
  );
}

