// Radios (every stick's port, host, baud and role), network, radio 1's write pacing, firmware source, API access,
// the admin password and updates.

import { useEffect, useState } from "preact/hooks";
import { auth, config, loadAuth, loadConfig, post, rebootNeeded, toast } from "../store";
import { Card, Field, Seg, copyText } from "../ui";
import { Icon } from "../icons";
import { confirmIt } from "../dialog";
import type { Config, UpdateTrain } from "../types";
import { Msg, RadiosSettingsCard } from "./Radios";

function useDraft(): [Config | null, (p: Partial<Config>) => void, () => void] {
  const [d, setD] = useState<Config | null>(config.value);
  useEffect(() => { if (!d && config.value) setD(config.value); }, [config.value]);
  return [d, (p) => setD((x) => (x ? { ...x, ...p } : x)), () => setD(config.value)];
}


async function save(patch: Partial<Config>, set: (m: { ok: boolean; text: string } | null) => void) {
  set(null);
  try {
    const r = await post<Config & { reboot_required: boolean }>("/api/config", patch);
    config.value = r;
    if (r.reboot_required) rebootNeeded.value = true;
    set({ ok: true, text: r.reboot_required ? "Saved; reboot to apply" : "Saved" });
    setTimeout(() => set(null), 4000);
  } catch (e: any) {
    set({ ok: false, text: e.message });
  }
}

function NetworkCard() {
  const [d, upd, reset] = useDraft();
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);
  if (!d) return <Card title="Network"><p class="muted">Loading…</p></Card>;
  const st = d.mode === "static";
  return (
    <Card title={<><Icon name="router" size={18} /> Network</>}>
      <form onSubmit={(e) => { e.preventDefault(); void save({ mode: d.mode, ip: d.ip.trim(), mask: d.mask.trim(), gw: d.gw.trim(), dns: d.dns.trim(), hostname: d.hostname.trim().toLowerCase() }, setM); }}>
        <div style={{ marginBottom: "12px" }}>
          <Seg label="Address" value={d.mode} onChange={(v) => upd({ mode: v })} options={[{ v: "dhcp", label: "DHCP" }, { v: "static", label: "Static IP" }]} />
        </div>
        <div class="form-grid two">
          <Field label="IP address"><input value={d.ip} disabled={!st} inputMode="decimal" onInput={(e) => upd({ ip: (e.target as HTMLInputElement).value })} /></Field>
          <Field label="Subnet mask"><input value={d.mask} disabled={!st} inputMode="decimal" onInput={(e) => upd({ mask: (e.target as HTMLInputElement).value })} /></Field>
          <Field label="Gateway"><input value={d.gw} disabled={!st} inputMode="decimal" onInput={(e) => upd({ gw: (e.target as HTMLInputElement).value })} /></Field>
          <Field label="DNS server"><input value={d.dns} disabled={!st} inputMode="decimal" onInput={(e) => upd({ dns: (e.target as HTMLInputElement).value })} /></Field>
          <span class="wide"><Field label="Hostname" help={<>Also reachable as <span class="mono">{d.hostname}.local</span></>}>
            <input value={d.hostname} onInput={(e) => upd({ hostname: (e.target as HTMLInputElement).value })} /></Field></span>
        </div>
        <div class="form-actions">
          <button class="btn primary">Save network</button>
          <button type="button" class="btn" onClick={reset}>Undo changes</button>
          <Msg m={m} />
        </div>
        <p class="field-help">A wrong static IP makes the bridge unreachable; 5 quick power cycles (under 10 s each) put it back on DHCP (find it as zigbee-eth-bridge.local or in your router's client list).</p>
      </form>
    </Card>
  );
}

function PacingCard() {
  const [d, upd, reset] = useDraft();
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);
  if (!d || !config.value) return <Card title="Radio 1 write pacing"><p class="muted">Loading…</p></Card>;
  if (d.tx_chunk === undefined) return null;  // firmware without pacing
  const baud = config.value.baud || 115200;
  const chunkMs = d.tx_chunk ? ((d.tx_chunk * 10 * 1000) / baud).toFixed(2) : null;
  return (
    <Card title={<><Icon name="upload" size={18} /> Radio 1 write pacing</>}>
      <p class="muted small" style={{ marginTop: 0 }}>How the bridge writes radio 1's traffic to its stick: in small chunks with a short gap, so the stick's serial line keeps up with bursts from Zigbee2MQTT. The defaults suit most sticks. Applies at once, no reboot.</p>
      <form onSubmit={(e) => { e.preventDefault(); void save({ tx_chunk: Number(d.tx_chunk), tx_gap_us: Number(d.tx_gap_us), xoff_honor: !!d.xoff_honor, xoff_max_ms: Number(d.xoff_max_ms), ...(d.hw_flow === undefined ? {} : { hw_flow: !!d.hw_flow }) }, setM); }}>
        <div class="form-grid two">
          <Field label="USB chunk" help={chunkMs ? `${chunkMs} ms on the line at ${baud.toLocaleString()} baud` : "Each write goes out whole, as before 1.0.17"}>
            <select value={String(d.tx_chunk)} onChange={(e) => upd({ tx_chunk: Number((e.target as HTMLSelectElement).value) })}>
              {[[64, "64 bytes"], [32, "32 bytes"], [16, "16 bytes"], [0, "Off"]].map(([v, l]) => <option value={v}>{l}</option>)}
            </select>
          </Field>
          <Field label="Gap between chunks (µs)" help="Idle line after the previous chunk has gone out. A lone frame isn't delayed.">
            <input type="number" min={0} max={5000} step={50} value={d.tx_gap_us} disabled={!d.tx_chunk} onInput={(e) => upd({ tx_gap_us: Number((e.target as HTMLInputElement).value) })} />
          </Field>
          <Field label="XOFF from the stick">
            <Seg label="XOFF" value={d.xoff_honor ? "hold" : "count"} onChange={(v) => upd({ xoff_honor: v === "hold" })} options={[{ v: "hold", label: "Hold writes" }, { v: "count", label: "Count only" }]} />
          </Field>
          <Field label="Longest hold (ms)" help="Without an XON, writing resumes after this.">
            <input type="number" min={1} max={200} value={d.xoff_max_ms} disabled={!d.xoff_honor} onInput={(e) => upd({ xoff_max_ms: Number((e.target as HTMLInputElement).value) })} />
          </Field>
          {d.hw_flow !== undefined && (
            <span class="wide"><Field label="Hardware flow control (CTS)" help="The USB chip stops sending while the stick drops its RTS. Refused unless the stick drives CTS now; turns itself off if a write stalls.">
              <Seg label="CTS" value={d.hw_flow ? "on" : "off"} onChange={(v) => upd({ hw_flow: v === "on" })} options={[{ v: "off", label: "Off" }, { v: "on", label: "Honour CTS" }]} />
            </Field></span>
          )}
        </div>
        <div class="form-actions">
          <button class="btn primary">Save</button>
          <button type="button" class="btn" onClick={reset}>Undo changes</button>
          <Msg m={m} />
        </div>
      </form>
    </Card>
  );
}

function FirmwareSourceCard() {
  const [d, upd, reset] = useDraft();
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);
  if (!d || !config.value || d.fw_repo === undefined) return null;
  return (
    <Card title={<><Icon name="refresh" size={18} /> Silicon Labs firmware source</>}>
      <p class="muted small" style={{ marginTop: 0 }}>Where System → Stick firmware looks for builds for Silicon Labs sticks (EFR32, like the MG21 and MG24): a GitHub repo whose releases use silabs-firmware-builder's file names (yours, if you build your own). TI sticks always use Koenkk/Z-Stack-firmware.</p>
      <form onSubmit={(e) => { e.preventDefault(); void save({ fw_repo: (d.fw_repo || "").trim(), fw_prerelease: !!d.fw_prerelease }, setM); }}>
        <div class="form-grid two">
          <Field label="GitHub repo (owner/name)"><input value={d.fw_repo} placeholder="Nerivec/silabs-firmware-builder" onInput={(e) => upd({ fw_repo: (e.target as HTMLInputElement).value })} /></Field>
          <Field label="Releases" help="Nerivec publishes each new SDK as a pre-release first.">
            <Seg label="Releases" value={d.fw_prerelease ? "pre" : "stable"} onChange={(v) => upd({ fw_prerelease: v === "pre" })} options={[{ v: "pre", label: "Include pre-releases" }, { v: "stable", label: "Latest release only" }]} />
          </Field>
        </div>
        <div class="form-actions"><button class="btn primary">Save</button><button type="button" class="btn" onClick={reset}>Undo changes</button><Msg m={m} /></div>
      </form>
    </Card>
  );
}

function TokenCard() {
  const [tok, setTok] = useState<string | null>(null);
  const has = auth.value?.api_token_set;
  const create = async () => {
    if (has && !(await confirmIt({ title: "Create a new API token?", body: "The current token stops working; Zigbee Studio needs the new one.", confirm: "Create token" }))) return;
    try {
      const r = await post<{ token: string }>("/api/token");
      setTok(r.token);
      toast("Token created. Copy it now; it won't be shown again.", "ok", 5000);
      void loadAuth();
    } catch (e: any) { toast(e.message, "error"); }
  };
  const revoke = async () => {
    if (!(await confirmIt({ title: "Revoke the API token?", body: "Zigbee Studio and any script using it lose access.", confirm: "Revoke", danger: true }))) return;
    try { await post("/api/token/revoke"); setTok(null); toast("Token revoked"); void loadAuth(); } catch (e: any) { toast(e.message, "error"); }
  };
  return (
    <Card title={<><Icon name="lock" size={18} /> API access</>} actions={has ? <span class="chip ok">Token active</span> : <span class="chip">No token</span>}>
      <p class="muted small" style={{ marginTop: 0 }}>Zigbee Studio and scripts read and change this bridge through the same JSON API as this page, sending
        <span class="mono"> Authorization: Bearer &lt;token&gt;</span>. The bridge also announces itself as <span class="mono">_zbbridge._tcp</span> on mDNS.</p>
      {tok && (
        <pre class="snippet token">{tok}
          <button type="button" class="icon-btn" title="Copy" aria-label="Copy" onClick={async () => toast((await copyText(tok)) ? "Copied" : "Copy failed")}><Icon name="copy" size={16} /></button>
        </pre>
      )}
      <div class="form-actions" style={{ marginTop: "10px" }}>
        <button type="button" class="btn primary" onClick={create}><Icon name="plus" size={15} /> {has ? "New token" : "Create token"}</button>
        {has && <button type="button" class="btn" onClick={revoke}>Revoke</button>}
      </div>
      <p class="field-help">Prometheus: <span class="mono">GET /metrics</span> with the token, or Basic auth as <span class="mono">admin</span>.</p>
    </Card>
  );
}

function PasswordCard() {
  const [cur, setCur] = useState("");
  const [a, setA] = useState("");
  const [b, setB] = useState("");
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);
  const submit = async (e: Event) => {
    e.preventDefault();
    if (a !== b) return setM({ ok: false, text: "The new passwords don't match" });
    try {
      await post("/api/password", { current: cur, new: a });
      setCur(""); setA(""); setB("");
      setM({ ok: true, text: "Password changed; other sessions were signed out" });
    } catch (x: any) { setM({ ok: false, text: x.message }); }
  };
  const logout = async () => { try { await post("/api/logout"); } catch { /* fine */ } void loadAuth(); };
  return (
    <Card title={<><Icon name="lock" size={18} /> Admin password</>}>
      <form onSubmit={submit}>
        <div class="form-grid two">
          <span class="wide"><Field label="Current password"><input type="password" autoComplete="current-password" value={cur} onInput={(e) => setCur((e.target as HTMLInputElement).value)} /></Field></span>
          <Field label="New password"><input type="password" autoComplete="new-password" value={a} onInput={(e) => setA((e.target as HTMLInputElement).value)} /></Field>
          <Field label="Repeat"><input type="password" autoComplete="new-password" value={b} onInput={(e) => setB((e.target as HTMLInputElement).value)} /></Field>
        </div>
        <div class="form-actions">
          <button class="btn primary" disabled={!cur || a.length < 8}>Change password</button>
          <button type="button" class="btn" onClick={logout}>Sign out</button>
          <Msg m={m} />
        </div>
      </form>
    </Card>
  );
}

const TRAIN_HELP: Record<UpdateTrain, string> = {
  stable: "The tested releases. What every bridge should run.",
  rc: "Release candidates too: each release is offered here first, for testing, and the release itself when it follows. Going back to Stable keeps the candidate until the next release is newer.",
  custom: "Your own release manifest, for firmware you build and host yourself.",
};

function UpdatesCard() {
  const [d, upd] = useDraft();
  const [m, setM] = useState<{ ok: boolean; text: string } | null>(null);
  if (!d) return <Card title="Updates" class="span-all"><p class="muted">Loading…</p></Card>;
  const train: UpdateTrain = d.update_train ?? "stable";
  const submit = async () => {
    const before = config.value?.update_train ?? "stable";
    await save({ update_train: train, update_url: d.update_url.trim(), update_auto: d.update_auto }, setM);
    // a new train: look for its releases at once instead of at the next scheduled check
    if (config.value?.update_train === train && train !== before) { try { await post("/api/update/check"); } catch { /* shown on System */ } }
  };
  return (
    <Card title={<><Icon name="refresh" size={18} /> Updates</>} class="span-all">
      <form onSubmit={(e) => { e.preventDefault(); void submit(); }}>
        <div class="form-grid update-form">
          <Field label="Release train" help={TRAIN_HELP[train]}>
            <Seg label="Release train" value={train} onChange={(v) => upd({ update_train: v as UpdateTrain })}
              options={[{ v: "stable", label: "Stable" }, { v: "rc", label: "Release candidates" }, { v: "custom", label: "Custom" }]} />
          </Field>
          {train === "custom" && (
            <Field label="Release manifest" help="The bridge reads this file to find new firmware; each release names its image with its MD5 and SHA-256, and the bridge checks both before switching. https only.">
              <input value={d.update_url} spellcheck={false} onInput={(e) => upd({ update_url: (e.target as HTMLInputElement).value })} />
            </Field>
          )}
          <Field label="Check by itself" help="One minute after starting, then every 6 hours. Installing always waits for you.">
            <Seg label="Automatic checks" value={d.update_auto ? "on" : "off"} onChange={(v) => upd({ update_auto: v === "on" })}
              options={[{ v: "on", label: "On" }, { v: "off", label: "Off" }]} />
          </Field>
        </div>
        <div class="form-actions"><button class="btn primary">Save</button><Msg m={m} /></div>
      </form>
    </Card>
  );
}

export function Settings() {
  useEffect(() => { void loadConfig(); }, []);
  return (
    <div class="page">
      <div class="page-head"><div><h1>Settings</h1><p class="lede">Saved on the bridge. Radio settings and pacing apply at once; network settings and full-speed-only USB after a reboot.</p></div></div>
      <div class="cols-2">
        <RadiosSettingsCard />
        <NetworkCard />
        <PacingCard />
        <FirmwareSourceCard />
        <TokenCard />
        <PasswordCard />
        <UpdatesCard />
      </div>
    </div>
  );
}
