// Updates from git, firmware (with upload), log storage, system facts, maintenance and the log (live + stored).

import { useEffect, useRef, useState } from "preact/hooks";
import { api, atUptime, awaitReboot, bootEpoch, logText, pollLog, post, resetEvents, status, toast } from "../store";
import { bytes, clock, duration, num } from "../format";
import type { DongleFw, DongleModel, FwCatalog, UpdateState } from "../types";
import { Card, Empty, KV, Seg, Stat } from "../ui";
import { LogStorage, StoredLog } from "./LogStore";
import { Icon } from "../icons";
import { confirmIt } from "../dialog";
import { rebootNow } from "../App";

// ESP-IDF log lines: "I (1234) tag: message", the number being ms since boot
const LINE = /^([EWIDV]) \((\d+)\) ([^:]+): (.*)$/;

/** Clock time of a log line (with the date when it isn't today); uptime seconds until the boot time is known. */
function logTime(ms: number): { text: string; title: string } {
  const up = ms / 1000;
  if (!bootEpoch.value) return { text: up.toFixed(1), title: `${duration(up)} after boot` };
  const ts = atUptime(up);
  const d = new Date(ts * 1000);
  const day = d.toDateString() === new Date().toDateString() ? "" : `${d.toLocaleDateString(undefined, { month: "short", day: "numeric" })} `;
  return { text: day + clock(ts), title: `${d.toLocaleString()} (${up.toFixed(1)} s after boot)` };
}

function LiveLog() {
  const box = useRef<HTMLDivElement>(null);
  const [follow, setFollow] = useState(true);
  useEffect(() => { void pollLog(true); }, []);
  const lines = logText.value.split("\n").filter(Boolean).slice(-600);
  useEffect(() => { if (follow && box.current) box.current.scrollTop = box.current.scrollHeight; });
  return (
    <>
      <div class="log-tools">
        <label class="check"><input type="checkbox" checked={follow} onChange={(e) => setFollow((e.target as HTMLInputElement).checked)} /> Follow</label>
        <span class="muted small">This boot, newest lines as they come (32 KB in memory)</span>
      </div>
      <div class="log sys-log" ref={box}>
        {lines.length === 0 ? <Empty>Waiting for log lines…</Empty> : lines.map((l) => {
          const m = l.match(LINE);
          if (!m) return <div class="log-line"><span class="log-msg muted">{l}</span></div>;
          const t = logTime(Number(m[2]));
          return (
            <div class={`log-line lv-${m[1].toLowerCase()}`}>
              <span class="log-ts" title={t.title}>{t.text}</span>
              <span class="log-lv">{m[1]}</span>
              <span class="log-ns">{m[3]}</span>
              <span class="log-msg">{m[4]}</span>
            </div>
          );
        })}
      </div>
    </>
  );
}

function Log() {
  const [tab, setTab] = useState<"live" | "stored">("live");
  return (
    <Card title={<><Icon name="terminal" size={18} /> Log</>} actions={
      <Seg label="Which log" value={tab} onChange={setTab} options={[{ v: "live", label: "Live" }, { v: "stored", label: "Stored" }]} />}>
      {tab === "live" ? <LiveLog /> : <StoredLog />}
    </Card>
  );
}

function Upload() {
  const [pct, setPct] = useState<number | null>(null);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const pick = async (f: File | undefined) => {
    if (!f) return;
    if (!(await confirmIt({ title: `Install ${f.name}?`, body: `${bytes(f.size)}. The bridge reboots into it and goes back to the current firmware by itself if the new one doesn't start.`, confirm: "Install" }))) return;
    setMsg(null);
    setPct(0);
    const x = new XMLHttpRequest();
    x.open("POST", "/api/ota");
    x.setRequestHeader("X-Bridge", "1");
    x.setRequestHeader("Content-Type", "application/octet-stream");
    x.upload.onprogress = (e) => e.lengthComputable && setPct((e.loaded / e.total) * 100);
    x.onload = () => {
      let j: any = {};
      try { j = JSON.parse(x.responseText); } catch { /* not JSON */ }
      if (x.status === 200) {
        setMsg({ ok: true, text: `Installed ${j.version}. Rebooting…` });
        awaitReboot("Rebooting into the new firmware…");
      } else {
        setPct(null);
        setMsg({ ok: false, text: j.error || `Upload failed (HTTP ${x.status})` });
      }
    };
    x.onerror = () => { setPct(null); setMsg({ ok: false, text: "Upload failed" }); };
    x.send(f);
  };
  return (
    <div style={{ marginTop: "12px" }}>
      <label class="btn file-btn">
        <Icon name="upload" size={15} /> Upload a file…
        <input type="file" class="sr-only" accept=".bin" onChange={(e) => { const i = e.target as HTMLInputElement; void pick(i.files?.[0]); i.value = ""; }} />
      </label>
      {pct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${pct}%` }} /></div><span class="small muted">{Math.round(pct)} %</span></div>}
      {msg && <p class={`form-msg ${msg.ok ? "ok" : "bad"}`}>{msg.text}</p>}
      <p class="field-help">Or install a <span class="mono">zigbee_eth_bridge.bin</span> from your computer. Other images are refused.</p>
    </div>
  );
}

/** Releases on git: check, then download, verify (size, MD5, SHA-256) and install on the bridge itself. */
function Updates() {
  const [u, setU] = useState<UpdateState | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const load = async () => {
    try { setU(await api<UpdateState>("/api/update")); setErr(null); } catch (e: any) { setErr(e.message); }
  };
  useEffect(() => {
    void load();
    const busy = u?.state === "checking" || u?.state === "downloading";
    const t = setInterval(load, busy ? 1000 : 5000);
    return () => clearInterval(t);
  }, [u?.state]);
  useEffect(() => { if (u?.state === "rebooting") awaitReboot(`Installed ${u.latest}; rebooting…`); }, [u?.state]);
  const check = async () => {
    try { await post("/api/update/check"); setU(u ? { ...u, state: "checking" } : u); } catch (e: any) { toast(e.message, "error"); }
  };
  const install = async () => {
    if (!u) return;
    if (!(await confirmIt({ title: `Install firmware ${u.latest}?`, confirm: "Install",
      body: <>
        {u.rolled_back === u.latest && <p class="bad-text" style={{ marginTop: 0 }}>{u.latest} was installed before and didn't start properly, so the bridge went back to {u.current}. It will most likely fail the same way.</p>}
        <p style={{ marginTop: 0 }}>The bridge downloads it from git ({bytes(u.size)}), checks its size, MD5 and SHA-256 against the release, reads it back from flash, and only then switches to it. Zigbee2MQTT loses its coordinator for about 10 seconds while it restarts; if the new firmware doesn't start, the bridge goes back to {u.current} by itself.</p>
        {u.notes && <p class="muted small" style={{ whiteSpace: "pre-wrap" }}>{u.notes}</p>}
      </> }))) return;
    try { await post("/api/update/install"); setU({ ...u, state: "downloading", done: 0 }); } catch (e: any) { toast(e.message, "error"); }
  };
  if (!u) return <Card title={<><Icon name="refresh" size={18} /> Updates</>} class="span-all">{err ? <p class="bad-text small">{err}</p> : <Empty>Loading…</Empty>}</Card>;
  const pct = u.size ? Math.min(100, (u.done / u.size) * 100) : 0;
  const checked = u.checked_ago >= 0 ? `checked ${duration(u.checked_ago)} ago` : "not checked yet";
  return (
    <Card title={<><Icon name="refresh" size={18} /> Updates</>} class="span-all"
      actions={<span class="muted small">{u.auto ? "checks git every 6 h" : "automatic checks off"} · {checked}</span>}>
      <div class="update-row">
        <div class="update-main">
          {u.state === "available" && <>
            <div class="update-head"><span class="chip warn">New</span> <b>Firmware {u.latest}</b> is available{u.date ? ` (${u.date})` : ""} · you have {u.current}</div>
            {u.notes && <p class="update-notes">{u.notes}</p>}
          </>}
          {u.rolled_back && <div class="update-head bad-text"><Icon name="alert" size={16} /> Firmware {u.rolled_back} didn't start properly, so the bridge went back to {u.current}.</div>}
          {u.state === "uptodate" && <div class="update-head"><span class="ok-text"><Icon name="check" size={16} /> Up to date</span> <span class="muted">· {u.current} is the latest release</span></div>}
          {u.state === "idle" && <div class="update-head muted">Not checked yet. You run {u.current}.</div>}
          {u.state === "checking" && <div class="update-head muted">Checking git…</div>}
          {u.state === "downloading" && <>
            <div class="update-head">Downloading and checking {u.latest}… <span class="muted">{bytes(u.done)} of {bytes(u.size)}</span></div>
            <div class="progress-wrap"><div class="progress"><i style={{ width: `${pct}%` }} /></div><span class="small muted">{Math.round(pct)} %</span></div>
          </>}
          {u.state === "rebooting" && <div class="update-head ok-text">Verified and installed {u.latest}. Rebooting…</div>}
          {u.state === "error" && <div class="update-head bad-text"><Icon name="alert" size={16} /> {u.error}</div>}
          {u.latest && u.md5 && (u.state === "available" || u.state === "downloading") && (
            <p class="field-help mono">MD5 {u.md5} · SHA-256 {u.sha256.slice(0, 16)}…</p>
          )}
        </div>
        <div class="row-btns update-btns">
          {(u.state === "available" || (u.state === "error" && u.latest && u.latest !== u.current)) && (
            <button type="button" class="btn primary" onClick={install}><Icon name="download" size={15} /> Install {u.latest}</button>
          )}
          <button type="button" class="btn" disabled={u.state === "checking" || u.state === "downloading" || u.state === "rebooting"} onClick={check}>
            <Icon name="refresh" size={15} /> Check now
          </button>
        </div>
      </div>
    </Card>
  );
}

const DONGLE_STEPS: Record<string, string> = {
  image: "Checking the stored image again",
  bootloader: "Getting the dongle into its bootloader",
  upload: "Sending the firmware to the dongle",
  start: "Starting the new firmware",
  verify: "Checking the new firmware answers",
};

/** The Zigbee dongle's own firmware: upload a .gbl (checked and kept on the bridge), then flash it. */
function DongleFirmware() {
  const [d, setD] = useState<DongleFw | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [pct, setPct] = useState<number | null>(null);
  const load = async () => {
    try { setD(await api<DongleFw>("/api/dongle")); setErr(null); } catch (e: any) { setErr(e.message); }
  };
  const running = d?.flash.state === "running";
  const fetching = !!d?.catalog && (d.catalog.checking || d.catalog.download.state === "running");
  useEffect(() => {
    void load();
    const t = setInterval(load, running || fetching ? 1000 : 5000);
    return () => clearInterval(t);
  }, [running, fetching]);

  const upload = (f: File | undefined) => {
    if (!f) return;
    if (!/\.(gbl|zip|hex|gcf)$/i.test(f.name)) { toast("Pick a firmware file: .gbl (Silicon Labs), .zip/.hex (TI Z-Stack), .GCF (ConBee) or .hex (nRF52840)", "error"); return; }
    if (d && f.size > d.max_size) { toast(`Too big: the bridge keeps images up to ${bytes(d.max_size)}`, "error"); return; }
    setPct(0);
    const x = new XMLHttpRequest();
    x.open("POST", `/api/dongle/image?name=${encodeURIComponent(f.name)}`);
    x.setRequestHeader("X-Bridge", "1");
    x.setRequestHeader("Content-Type", "application/octet-stream");
    x.upload.onprogress = (e) => e.lengthComputable && setPct((e.loaded / e.total) * 100);
    x.onload = () => {
      setPct(null);
      let j: any = {};
      try { j = JSON.parse(x.responseText); } catch { /* not JSON */ }
      if (x.status === 200) { setD(j); toast(`${f.name} checked and stored`); }
      else toast(j.error || `Upload failed (HTTP ${x.status})`, "error", 8000);
    };
    x.onerror = () => { setPct(null); toast("Upload failed", "error"); };
    x.send(f);
  };

  const flash = async () => {
    if (!d?.image) return;
    const im = d.image;
    if (!(await confirmIt({ title: `Flash ${im.version ? `EmberZNet ${im.version}` : im.name} to the dongle?`, confirm: "Flash the dongle",
      body: <>
        <p style={{ marginTop: 0 }}>Zigbee2MQTT loses its coordinator while this runs (usually 1–2 minutes) and reconnects by itself afterwards. The dongle now runs {d.current ?? "firmware the bridge hasn't seen yet"}.</p>
        <p>The bridge checks the image again, asks the dongle to start its bootloader, sends the firmware, starts it and makes sure it answers. The Zigbee network lives in the dongle's own storage, not in the firmware, so it normally carries over; Zigbee2MQTT can also restore it from its coordinator backup.</p>
        <p class="muted small">If it fails partway the dongle may be left in its bootloader without firmware. Nothing is lost: flash again from here, the image stays on the bridge.</p>
      </> }))) return;
    try { setD(await post<DongleFw>("/api/dongle/flash", { md5: im.md5 })); } catch (e: any) { toast(e.message, "error", 8000); void load(); }
  };

  const remove = async () => {
    if (!d?.image || !(await confirmIt({ title: `Remove ${d.image.name}?`, body: "Deletes the stored image from the bridge. The dongle isn't touched.", confirm: "Remove" }))) return;
    try { setD(await post<DongleFw>("/api/dongle/image/delete")); } catch (e: any) { toast(e.message, "error"); }
  };

  const title = <><Icon name="coordinator" size={18} /> Dongle firmware</>;
  if (!d) return <Card title={title} class="span-all">{err ? <p class="bad-text small">{err}</p> : <Empty>Loading…</Empty>}</Card>;
  const f = d.flash, im = d.image;
  const upPct = f.step === "upload" && f.total ? Math.min(100, ((f.done ?? 0) / f.total) * 100) : null;
  const blocked = !d.dongle ? "No dongle connected" : im?.problem ? "This image can't go on the coordinator" : d.busy ? "Busy" : "";
  return (
    <Card title={title} class="span-all" actions={<span class="muted small">{d.current ? `Dongle runs ${d.current}` : d.dongle ? "version shows once Zigbee2MQTT connects" : "no dongle"}</span>}>
      <div class="update-row">
        <div class="update-main">
          {running && <>
            <div class="update-head">{DONGLE_STEPS[f.step ?? ""] ?? "Working"}… <span class="muted">{duration(f.seconds ?? 0)}</span></div>
            {upPct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${upPct}%` }} /></div><span class="small muted">{Math.round(upPct)} %</span></div>}
            <p class="field-help">Zigbee2MQTT is cut off until this finishes. Don't unplug the dongle or the bridge.</p>
          </>}
          {f.state === "done" && <div class="update-head ok-text"><Icon name="check" size={16} /> Flashed in {duration(f.seconds ?? 0)}{f.before ? `: ${f.before} → ${f.after}` : `: now ${f.after}`}</div>}
          {f.state === "failed" && <>
            <div class="update-head bad-text"><Icon name="alert" size={16} /> {f.error}</div>
            <p class="field-help">{f.dongle_left === "running" ? `The dongle still runs its previous firmware${f.after ? ` (${f.after})` : ""}; Zigbee2MQTT can reconnect.`
              : f.dongle_left === "bootloader" ? "The dongle is in its bootloader with no working firmware. Flash again (the image is still here) or recover it from a PC."
              : "Check the dongle before relying on it."}</p>
          </>}
          {im ? <>
            <div class="update-head"><b>{im.version ? `EmberZNet ${im.version}` : im.name}</b> <span class="muted">stored on the bridge{im.kind && im.kind !== "Silicon Labs .gbl" ? ` · ${im.kind}` : ""}</span></div>
            {im.summary && <p class="field-help">{im.summary}</p>}
            <p class="field-help">{im.name} · {bytes(im.size)}{im.uploaded ? ` · uploaded ${new Date(im.uploaded * 1000).toLocaleString()}` : ""}</p>
            {(!im.kind || im.kind === "Silicon Labs .gbl") && <p class="field-help">{[im.fw_type, im.sdk && `SDK ${im.sdk}`, im.image_baud && `${im.image_baud} baud`, im.signed && "signed", im.encrypted && "encrypted"].filter(Boolean).join(" · ") || "No build metadata in the image"}</p>}
            <p class="field-help mono">MD5 {im.md5} · SHA-256 {im.sha256.slice(0, 16)}…</p>
            {im.problem && <p class="bad-text small"><Icon name="alert" size={14} /> {im.problem}</p>}
            {!im.problem && (im.sets_baud || im.sets_hw_flow !== undefined) && <p class="small muted"><Icon name="info" size={14} /> Flashing it also switches the bridge
              {im.sets_baud ? ` to ${num(im.sets_baud)} baud` : ""}{im.sets_baud && im.sets_hw_flow !== undefined ? " and" : ""}
              {im.sets_hw_flow !== undefined ? ` turns hardware flow control ${im.sets_hw_flow ? "on" : "off"}` : ""}, once the new firmware answers. No reboot needed.</p>}
          </> : !running && <div class="update-head muted">No dongle firmware stored. Download a build below or upload a Zigbee coordinator (NCP) .gbl.</div>}
          {pct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${pct}%` }} /></div><span class="small muted">Uploading {Math.round(pct)} %</span></div>}
          <p class="field-help">Checked on the bridge before the dongle is touched: GBL structure, CRC-32, an MD5 read back from flash, Zigbee coordinator firmware, no bootloader or secure-element changes. A build at another baud is verified at its own baud and the bridge switches to it.</p>
        </div>
        <div class="row-btns update-btns">
          {im && <button type="button" class="btn primary" disabled={!!blocked || running} title={blocked} onClick={flash}><Icon name="zigbee" size={15} /> Flash the dongle</button>}
          <label class={`btn file-btn${running || pct !== null ? " disabled" : ""}`}>
            <Icon name="upload" size={15} /> {im ? "Replace…" : "Upload firmware…"}
            <input type="file" class="sr-only" accept=".gbl,.zip,.hex,.gcf" disabled={running || pct !== null} onChange={(e) => { const i = e.target as HTMLInputElement; upload(i.files?.[0]); i.value = ""; }} />
          </label>
          {im && <button type="button" class="btn" disabled={running} onClick={remove}><Icon name="trash" size={15} /> Remove</button>}
        </div>
      </div>
      {d.catalog && <FirmwareCatalog c={d.catalog} baud={d.baud} busy={running || pct !== null || d.busy} onChange={setD} />}
    </Card>
  );
}

function fwLabel(type?: string) {
  return type === "openthread_rcp" ? "OpenThread RCP" : type === "coordinator" ? "Z-Stack" : "EmberZNet";
}

function FirmwareCatalog({ c, baud, busy, onChange }: { c: FwCatalog; baud: number; busy: boolean; onChange: (d: DongleFw) => void }) {
  const check = async () => { try { onChange(await post<DongleFw>("/api/dongle/firmware/check")); } catch (e: any) { toast(e.message, "error"); } };
  const download = async (name: string) => {
    if (!(await confirmIt({ title: `Download ${name}?`, confirm: "Download",
      body: <><p style={{ marginTop: 0 }}>The bridge downloads it from GitHub ({c.repo}) into its dongle image area, replacing the stored image, and checks it (GitHub's SHA-256, GBL structure, CRC-32, MD5 read-back).</p>
        <p>The dongle isn't touched: flashing stays a separate step.</p></> }))) return;
    try { onChange(await post<DongleFw>("/api/dongle/firmware/download", { name })); } catch (e: any) { toast(e.message, "error", 8000); }
  };
  const dl = c.download;
  const dlPct = dl.state === "running" && dl.total ? Math.min(100, (dl.done / dl.total) * 100) : null;
  const best = c.builds.find((b) => b.recommended);
  return (
    <div class="fw-catalog">
      <div class="fw-catalog-head">
        <h4>Firmware updates{c.board ? <span class="muted small"> · {c.model} (<span class="mono">{c.board}</span>)</span> : null}</h4>
        <button type="button" class="btn" disabled={c.checking || busy} onClick={check}><Icon name="refresh" size={15} /> {c.checking ? "Checking…" : "Check GitHub"}</button>
      </div>
      <p class="field-help">
        {c.current ? <>Dongle runs {fwLabel(c.type)} <b>{c.current}</b>. </> : null}
        {c.release ? <>Newest {c.prerelease ? "pre-release" : "release"} with builds for this dongle: <span class="mono">{c.release}</span> on <span class="mono">{c.repo}</span>. </> : null}
        {c.checked_ago >= 0 ? `Checked ${duration(c.checked_ago)} ago.` : c.checking ? "" : "Not checked yet."}
      </p>
      {c.error && <p class="bad-text small"><Icon name="alert" size={14} /> {c.error}</p>}
      {best && best.vs_current === "newer" && <p class="small ok-text"><Icon name="check" size={14} /> {fwLabel(c.type)} {best.version} is newer than what the dongle runs.</p>}
      {best && best.vs_current === "same" && <p class="small muted">The recommended build is the version the dongle already runs.</p>}
      {c.builds.length > 0 && <div class="table-wrap"><table class="data">
        <thead><tr><th>Build</th><th class="num">Baud</th><th>Flow</th><th class="num">Size</th><th /></tr></thead>
        <tbody>{c.builds.map((b) => (
          <tr class={b.usable ? "" : "muted"}>
            <td>{fwLabel(c.type)} {b.version}{b.sdk ? <span class="muted small"> SDK {b.sdk}</span> : null}{b.recommended ? <span class="chip ok" style={{ marginLeft: "6px" }}>Recommended</span> : null}
              {b.vs_current === "newer" ? <span class="chip info" style={{ marginLeft: "6px" }}>Newer</span> : null}
              {!b.usable ? <div class="small">Needs RTS/CTS, which this dongle doesn't have</div>
                : b.baud !== baud ? <div class="small muted">Talks at {num(b.baud)} baud: flashing it switches the bridge from {num(baud)}</div> : null}</td>
            <td class="num">{num(b.baud)}</td><td class="mono small">{b.flow}</td><td class="num">{bytes(b.size)}</td>
            <td class="num"><button type="button" class="btn small-btn" disabled={!b.usable || busy || dl.state === "running"} onClick={() => download(b.name)}>Download</button></td>
          </tr>))}</tbody>
      </table></div>}
      {dlPct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${dlPct}%` }} /></div><span class="small muted">Downloading {Math.round(dlPct)} %</span></div>}
      {dl.state === "failed" && <p class="bad-text small"><Icon name="alert" size={14} /> {dl.name}: {dl.error}</p>}
      {dl.state === "done" && <p class="ok-text small"><Icon name="check" size={14} /> {dl.name} downloaded and checked: it's the stored image above, ready to flash.</p>}
      <p class="field-help">{c.type === "coordinator" ? "Z-Stack coordinator builds from Koenkk/Z-Stack-firmware for this board." : `${c.type === "openthread_rcp" ? "OpenThread RCP" : "Coordinator (zigbee_ncp)"} builds for this board; the dongle role, source repo and pre-releases are on the Settings page.`} Any image can still be uploaded by hand.</p>
      {c.dongle_list && c.dongle_list.fetched_ago >= 0 && <p class="field-help">Dongle list from git: {c.dongle_list.status}.</p>}
    </div>
  );
}

function SupportedDongles() {
  const [list, setList] = useState<DongleModel[] | null>(null);
  const [open, setOpen] = useState(false);
  useEffect(() => { if (open && !list) api<DongleModel[]>("/api/dongles").then(setList).catch(() => setList([])); }, [open]);
  return (
    <Card title={<><Icon name="coordinator" size={18} /> Supported dongles</>} class="span-all"
      actions={<button type="button" class="link-btn" onClick={() => setOpen(!open)}>{open ? "Hide" : "Show list"}</button>}>
      <p class="muted small" style={{ marginTop: 0 }}>Recognised by USB ID and name. Any CP210x, CH340, FTDI or USB CDC serial adapter is driven even when it isn't listed; its protocol is detected from the traffic.
        Firmware can be flashed on Silicon Labs (EFR32) dongles; TI, ConBee and Nordic ones run as bridges only for now.</p>
      {open && (list ? <div class="table-wrap"><table class="data">
        <thead><tr><th>Dongle</th><th>Radio</th><th>USB</th><th>Protocol</th><th>Flash here</th></tr></thead>
        <tbody>{list.map((m) => (
          <tr><td>{m.name}{m.downloaded ? <span class="chip info" style={{ marginLeft: "6px" }}>from git</span> : null}{m.note ? <div class="muted small">{m.note}</div> : null}</td><td>{m.radio}</td>
            <td class="mono small">{m.usb_id} {m.usb_chip}</td><td>{m.protocol}{m.rtscts_wired ? <span class="muted small"> · RTS/CTS</span> : null}</td>
            <td>{m.flashable ? <span class="ok-text">Yes</span> : <span class="muted">No</span>}</td></tr>))}</tbody>
      </table></div> : <Empty>Loading…</Empty>)}
    </Card>
  );
}

export function System() {
  const s = status.value;
  if (!s) return <div class="page"><Empty>Loading…</Empty></div>;
  const resetStats = async () => {
    if (!(await confirmIt({ title: "Reset statistics?", body: "Counters, charts and the device table start again from zero. Events stay.", confirm: "Reset" }))) return;
    try { await post("/api/stats/reset"); toast("Statistics reset"); } catch (e: any) { toast(e.message, "error"); }
  };
  const factory = async () => {
    if (!(await confirmIt({ title: "Factory reset?", danger: true, typeToConfirm: "RESET", confirm: "Factory reset",
      body: "Clears every setting, the admin password and the API token, then reboots. The next visit shows first-boot setup. The network goes back to DHCP (find it as zigbee-eth-bridge.local or in your router's client list)." }))) return;
    try { await post("/api/factory-reset", { confirm: "RESET" }); resetEvents(); awaitReboot("Factory reset; rebooting…"); } catch (e: any) { toast(e.message, "error"); }
  };
  return (
    <div class="page">
      <div class="page-head"><div><h1>System</h1><p class="lede">{s.sys.chip} · ESP-IDF {s.fw.idf}</p></div></div>
      <div class="stats">
        <Stat label="Firmware" value={s.fw.version} sub={s.fw.pending_verify ? "on trial" : `running from ${s.fw.partition}`} tone={s.fw.pending_verify ? "warn" : ""} />
        <Stat label="Uptime" value={duration(s.uptime)} sub={s.sys.watchdog ? `restarted by the watchdog: ${s.sys.watchdog}` : `last reboot: ${s.sys.reset_reason}`}
          tone={/crash|watchdog|brownout/i.test(s.sys.reset_reason) ? "warn" : ""} />
        <Stat label="Chip temperature" value={`${s.sys.temp_c.toFixed(1)} °C`} sub={s.sys.temp_c > 65 ? "running hot" : "normal"}
          tone={s.sys.temp_c > 75 ? "bad" : s.sys.temp_c > 65 ? "warn" : ""} />
        <Stat label="Free memory" value={bytes(s.sys.heap_free)} sub={`lowest ${bytes(s.sys.heap_min)}${s.sys.psram_total ? ` · PSRAM ${bytes(s.sys.psram_free ?? 0)} of ${bytes(s.sys.psram_total)} free` : ""}`} />
      </div>
      <div class="cols-3">
        <Updates />
        <DongleFirmware />
        <SupportedDongles />
        <LogStorage />
        <Card title={<><Icon name="download" size={18} /> Firmware</>}>
          <KV rows={[["Version", s.fw.version], ["Built", s.fw.built], ["ESP-IDF", s.fw.idf], ["Running from", s.fw.partition]]} />
          <Upload />
        </Card>
        <Card title={<><Icon name="router" size={18} /> Network</>}>
          <KV rows={[
            ["Ethernet", s.eth.link ? `${s.eth.speed} Mbps ${s.eth.full_duplex ? "full" : "half"} duplex` : <span class="bad-text">Link down</span>],
            ["IP", `${s.eth.ip} (${s.eth.mode === "static" ? "static" : "DHCP"})`],
            ["Gateway", s.eth.gw],
            ["MAC", <span class="mono">{s.eth.mac}</span>],
            ["Hostname", `${s.eth.hostname}.local`],
            ["Clock", s.epoch ? new Date(s.epoch * 1000).toLocaleString() : <span class="muted">not synced</span>],
            ["Watchdog", s.sys.watchdog ? <span class="warn-text">last restart: {s.sys.watchdog}</span> : <span class="ok-text">armed, no restarts</span>],
          ]} />
        </Card>
        <Card title={<><Icon name="gear" size={18} /> Maintenance</>}>
          <div class="actions-list">
            <div><button type="button" class="btn" onClick={rebootNow}><Icon name="power" size={15} /> Reboot</button><span class="muted small">About 10 s without the coordinator.</span></div>
            <div><button type="button" class="btn" onClick={resetStats}><Icon name="refresh" size={15} /> Reset statistics</button></div>
            <div><button type="button" class="btn danger-text" onClick={factory}><Icon name="trash" size={15} /> Factory reset…</button><span class="muted small">Same as 5 quick power cycles (under 10 s each).</span></div>
          </div>
        </Card>
      </div>
      <Log />
    </div>
  );
}
