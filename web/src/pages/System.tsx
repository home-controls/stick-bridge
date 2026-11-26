// Updates from git, firmware (with upload), log storage, system facts, maintenance and the log (live + stored).

import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { api, atUptime, awaitReboot, bootEpoch, logText, pollLog, post, radioAt, resetEvents, status, toast, usePoll } from "../store";
import { bytes, clock, duration, num } from "../format";
import type { DongleFw, DongleModel, FwCatalog, UpdateState } from "../types";
import { Card, Empty, KV, Seg, Stat } from "../ui";
import { LogStorage, StoredLog } from "./LogStore";
import { Icon } from "../icons";
import { confirmIt } from "../dialog";
import { rebootNow } from "../App";
import { UsbCard, cap, clientOf, stickName } from "./Radios";

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
  // Parsed once per new log text, not on every status poll that re-renders the page
  const text = logText.value, boot = bootEpoch.value;
  const lines = useMemo(() => logText.value.split("\n").filter(Boolean).slice(-600).map((l) => {
    const m = l.match(LINE);
    return m ? { l, m, t: logTime(Number(m[2])) } : { l, m: null, t: null };
  }), [text, boot]);
  useEffect(() => { if (follow && box.current) box.current.scrollTop = box.current.scrollHeight; });
  return (
    <>
      <div class="log-tools">
        <label class="check"><input type="checkbox" checked={follow} onChange={(e) => setFollow((e.target as HTMLInputElement).checked)} /> Follow</label>
        <span class="muted small">This boot, newest lines as they come (32 KB in memory)</span>
      </div>
      <div class="log sys-log" ref={box}>
        {lines.length === 0 ? <Empty>Waiting for log lines…</Empty> : lines.map(({ l, m, t }) => {
          if (!m || !t) return <div class="log-line"><span class="log-msg muted">{l}</span></div>;
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
  const busy = u?.state === "checking" || u?.state === "downloading";
  usePoll(load, busy ? 1000 : 5000);
  useEffect(() => { if (u?.state === "rebooting") awaitReboot(`Installed ${u.latest}; rebooting…`); }, [u?.state]);
  const check = async () => {
    try { await post("/api/update/check"); setU(u ? { ...u, state: "checking" } : u); } catch (e: any) { toast(e.message, "error"); }
  };
  const install = async () => {
    if (!u) return;
    if (!(await confirmIt({ title: `Install firmware ${u.latest}?`, confirm: "Install",
      body: <>
        {u.rolled_back === u.latest && <p class="bad-text" style={{ marginTop: 0 }}>{u.latest} was installed before and didn't start properly, so the bridge went back to {u.current}. It will most likely fail the same way.</p>}
        <p style={{ marginTop: 0 }}>The bridge downloads it from git ({bytes(u.size)}), checks its size, MD5 and SHA-256 against the release, reads it back from flash, and only then switches to it. every radio's client (Zigbee2MQTT and the others) loses its stick for about 10 seconds while it restarts; if the new firmware doesn't start, the bridge goes back to {u.current} by itself.</p>
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
  bootloader: "Getting the stick into its bootloader",
  upload: "Sending the firmware to the stick",
  start: "Starting the new firmware",
  verify: "Checking the new firmware answers",
};

/** A radio's stick firmware: upload or download an image (checked and kept on the bridge), then flash it. The
 *  same card for every radio; the picker chooses whose stick. */
function DongleFirmware() {
  const [d, setD] = useState<DongleFw | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [pct, setPct] = useState<number | null>(null);
  const load = async () => {
    try { setD(await api<DongleFw>("/api/dongle")); setErr(null); } catch (e: any) { setErr(e.message); }
  };
  const running = d?.flash.state === "running";
  const fetching = !!d?.catalog && (d.catalog.checking || d.catalog.download.state === "running");
  usePoll(load, running || fetching ? 1000 : 5000);

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
    if (!(await confirmIt({ title: `Flash ${imLabel(im)} to radio ${picked + 1}'s stick?`, confirm: "Flash the stick",
      body: <>
        <p style={{ marginTop: 0 }}>{cap(who)} loses its stick while this runs (usually 1–2 minutes) and reconnects by itself afterwards. The stick now runs {d.current ?? "firmware the bridge hasn't seen yet"}.</p>
        <p>The bridge checks the image again, asks the stick to start its bootloader, sends the firmware, starts it and makes sure it answers. The network lives in the stick's own storage, not in the firmware, so it normally carries over; {who} can also restore it from its own backup.</p>
        <p class="muted small">If it fails partway the stick may be left in its bootloader without firmware. Nothing is lost: flash again from here, the image stays on the bridge.</p>
      </> }))) return;
    try { setD(await post<DongleFw>(`/api/dongle/flash?radio=${picked}`, { md5: im.md5 })); } catch (e: any) { toast(e.message, "error", 8000); void load(); }
  };

  const remove = async () => {
    if (!d?.image || !(await confirmIt({ title: `Remove ${d.image.name}?`, body: "Deletes the stored image from the bridge. No stick is touched.", confirm: "Remove" }))) return;
    try { setD(await post<DongleFw>("/api/dongle/image/delete")); } catch (e: any) { toast(e.message, "error"); }
  };

  // Which radio's stick this card works on (the shown radios)
  const radios = status.value?.radios ?? [];
  const picked = d?.radio ?? 0;
  const lr = radioAt(picked);
  const who = clientOf(lr?.role ?? "zigbee");
  const pick = async (i: number) => {
    try { setD(await api<DongleFw>(`/api/dongle?radio=${i}`)); } catch (e: any) { toast(e.message, "error"); }
  };
  const title = <><Icon name="coordinator" size={18} /> Stick firmware</>;
  const picker = radios.length > 1 && (
    <Seg label="Radio" value={String(picked)} onChange={(v) => void pick(Number(v))}
      options={radios.map((r) => ({ v: String(r.index), label: `${r.index + 1}. ${r.name}` }))} />
  );
  if (!d) return <Card title={title} class="span-all">{err ? <p class="bad-text small">{err}</p> : <Empty>Loading…</Empty>}</Card>;
  const f = d.flash, im = d.image;
  const upPct = f.step === "upload" && f.total ? Math.min(100, ((f.done ?? 0) / f.total) * 100) : null;
  const blocked = !d.dongle ? "No stick connected" : im?.problem ? "This image can't go on this stick" : d.busy ? "Busy" : "";
  const stick = lr?.stick.present ? stickName(lr) : "";
  const head = `Radio ${picked + 1} · ${!d.dongle || !stick ? "no stick" : d.current ? `${stick} runs ${d.current}` : `${stick}: version shows once its client has connected`}`;
  // Z-Wave controllers: one line, like a failed check
  const unsupported = lr?.role === "zwave" || /Z-Wave/.test(lr?.stick.protocol ?? "");
  return (
    <Card title={title} class="span-all" actions={picker || undefined}>
      <p class="update-head" style={{ marginTop: 0 }}><b>{head}</b></p>
      {unsupported ? <div class="fw-catalog"><p class="muted small"><Icon name="info" size={14} /> No firmware updates through the bridge for this stick.</p></div> : <>
      <div class="update-row">
        <div class="update-main">
          {running && <>
            <div class="update-head">{DONGLE_STEPS[f.step ?? ""] ?? "Working"}… <span class="muted">{duration(f.seconds ?? 0)}</span></div>
            {upPct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${upPct}%` }} /></div><span class="small muted">{Math.round(upPct)} %</span></div>}
            <p class="field-help">{cap(who)} is cut off until this finishes. Don't unplug the stick or the bridge.</p>
          </>}
          {f.state === "done" && <div class="update-head ok-text"><Icon name="check" size={16} /> Flashed in {duration(f.seconds ?? 0)}{f.before ? `: ${f.before} → ${f.after}` : `: now ${f.after}`}</div>}
          {f.state === "failed" && <>
            <div class="update-head bad-text"><Icon name="alert" size={16} /> {f.error}</div>
            <p class="field-help">{f.dongle_left === "running" ? `The stick still runs its previous firmware${f.after ? ` (${f.after})` : ""}; ${who} can reconnect.`
              : f.dongle_left === "bootloader" ? "The stick is in its bootloader with no working firmware. Flash again (the image is still here) or recover it from a PC."
              : "Check the stick before relying on it."}</p>
          </>}
          {im ? <>
            <div class="update-head"><b>{imLabel(im)}</b> <span class="muted">stored on the bridge{im.kind && im.kind !== "Silicon Labs .gbl" ? ` · ${im.kind}` : ""}</span></div>
            {im.summary && <p class="field-help">{im.summary}</p>}
            <p class="field-help">{im.name} · {bytes(im.size)}{im.uploaded ? ` · uploaded ${new Date(im.uploaded * 1000).toLocaleString()}` : ""}</p>
            {(!im.kind || im.kind === "Silicon Labs .gbl") && <p class="field-help">{[im.fw_type, im.sdk && `SDK ${im.sdk}`, im.image_baud && `${im.image_baud} baud`, im.signed && "signed", im.encrypted && "encrypted"].filter(Boolean).join(" · ") || "No build metadata in the image"}</p>}
            <p class="field-help mono">MD5 {im.md5} · SHA-256 {im.sha256.slice(0, 16)}…</p>
            {im.problem && <p class="bad-text small"><Icon name="alert" size={14} /> {im.problem}</p>}
            {!im.problem && (im.sets_baud || im.sets_hw_flow !== undefined) && <p class="small muted"><Icon name="info" size={14} /> Flashing it also switches the bridge
              {im.sets_baud ? ` to ${num(im.sets_baud)} baud` : ""}{im.sets_baud && im.sets_hw_flow !== undefined ? " and" : ""}
              {im.sets_hw_flow !== undefined ? ` turns hardware flow control ${im.sets_hw_flow ? "on" : "off"}` : ""}, once the new firmware answers. No reboot needed.</p>}
          </> : !running && <div class="update-head muted">No stick firmware stored. Download a build below or upload an image for this stick.</div>}
          {pct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${pct}%` }} /></div><span class="small muted">Uploading {Math.round(pct)} %</span></div>}
          <p class="field-help">Checked on the bridge before the stick is touched: the image's structure and checksums, an MD5 read back from flash, firmware that fits the radio's role, no bootloader or secure-element changes. A build at another baud is verified at its own baud and the bridge switches to it.</p>
        </div>
        <div class="row-btns update-btns">
          {im && <button type="button" class="btn primary" disabled={!!blocked || running} title={blocked} onClick={flash}><Icon name="zigbee" size={15} /> Flash the stick</button>}
          <label class={`btn file-btn${running || pct !== null ? " disabled" : ""}`}>
            <Icon name="upload" size={15} /> {im ? "Replace…" : "Upload firmware…"}
            <input type="file" class="sr-only" accept=".gbl,.zip,.hex,.gcf" disabled={running || pct !== null} onChange={(e) => { const i = e.target as HTMLInputElement; upload(i.files?.[0]); i.value = ""; }} />
          </label>
          {im && <button type="button" class="btn" disabled={running} onClick={remove}><Icon name="trash" size={15} /> Remove</button>}
        </div>
      </div>
      {d.catalog && <FirmwareCatalog c={d.catalog} radio={picked} baud={d.baud} busy={running || pct !== null || d.busy} onChange={setD} />}
      </>}
    </Card>
  );
}

function imLabel(im: NonNullable<DongleFw["image"]>) {
  return im.version ? `${im.fw_type === "openthread_rcp" ? "OpenThread RCP" : im.kind && /Z-Stack|TI/.test(im.kind) ? "Z-Stack" : "EmberZNet"} ${im.version}` : im.name;
}

function fwLabel(type?: string) {
  return type === "openthread_rcp" ? "OpenThread RCP" : type === "coordinator" ? "Z-Stack" : "EmberZNet";
}

function FirmwareCatalog({ c, radio, baud, busy, onChange }: { c: FwCatalog; radio: number; baud: number; busy: boolean; onChange: (d: DongleFw) => void }) {
  const check = async () => { try { onChange(await post<DongleFw>(`/api/dongle/firmware/check?radio=${radio}`)); } catch (e: any) { toast(e.message, "error"); } };
  const download = async (name: string) => {
    if (!(await confirmIt({ title: `Download ${name}?`, confirm: "Download",
      body: <><p style={{ marginTop: 0 }}>The bridge downloads it from GitHub ({c.repo}) into its stick image area, replacing the stored image, and checks it (GitHub's SHA-256, GBL structure, CRC-32, MD5 read-back).</p>
        <p>The stick isn't touched: flashing stays a separate step.</p></> }))) return;
    try { onChange(await post<DongleFw>(`/api/dongle/firmware/download?radio=${radio}`, { name })); } catch (e: any) { toast(e.message, "error", 8000); }
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
        {c.current ? <>The stick runs {fwLabel(c.type)} <b>{c.current}</b>. </> : null}
        {c.release ? <>Newest {c.prerelease ? "pre-release" : "release"} with builds for this stick: <span class="mono">{c.release}</span> on <span class="mono">{c.repo}</span>. </> : null}
        {c.checked_ago >= 0 ? `Checked ${duration(c.checked_ago)} ago.` : c.checking ? "" : "Not checked yet."}
      </p>
      {c.error && <p class="bad-text small"><Icon name="alert" size={14} /> {c.error}</p>}
      {best && best.vs_current === "newer" && <p class="small ok-text"><Icon name="check" size={14} /> {fwLabel(c.type)} {best.version} is newer than what the stick runs.</p>}
      {best && best.vs_current === "same" && <p class="small muted">The recommended build is the version the stick already runs.</p>}
      {c.builds.length > 0 && <div class="table-wrap"><table class="data">
        <thead><tr><th>Build</th><th class="num">Baud</th><th>Flow</th><th class="num">Size</th><th /></tr></thead>
        <tbody>{c.builds.map((b) => (
          <tr class={b.usable ? "" : "muted"}>
            <td>{fwLabel(c.type)} {b.version}{b.sdk ? <span class="muted small"> SDK {b.sdk}</span> : null}{b.recommended ? <span class="chip ok" style={{ marginLeft: "6px" }}>Recommended</span> : null}
              {b.vs_current === "newer" ? <span class="chip info" style={{ marginLeft: "6px" }}>Newer</span> : null}
              {!b.usable ? <div class="small">Needs RTS/CTS, which this stick doesn't have</div>
                : b.baud !== baud ? <div class="small muted">Talks at {num(b.baud)} baud: flashing it switches the bridge from {num(baud)}</div> : null}</td>
            <td class="num">{num(b.baud)}</td><td class="mono small">{b.flow}</td><td class="num">{bytes(b.size)}</td>
            <td class="num"><button type="button" class="btn small-btn" disabled={!b.usable || busy || dl.state === "running"} onClick={() => download(b.name)}>Download</button></td>
          </tr>))}</tbody>
      </table></div>}
      {dlPct !== null && <div class="progress-wrap"><div class="progress"><i style={{ width: `${dlPct}%` }} /></div><span class="small muted">Downloading {Math.round(dlPct)} %</span></div>}
      {dl.state === "failed" && <p class="bad-text small"><Icon name="alert" size={14} /> {dl.name}: {dl.error}</p>}
      {dl.state === "done" && <p class="ok-text small"><Icon name="check" size={14} /> {dl.name} downloaded and checked: it's the stored image above, ready to flash.</p>}
      <p class="field-help">{c.type === "coordinator" ? "Z-Stack coordinator builds from Koenkk/Z-Stack-firmware for this board." : `${c.type === "openthread_rcp" ? "OpenThread RCP" : "Coordinator (zigbee_ncp)"} builds for this board; the radio's role, the source repo and pre-releases are on the Settings page.`} Any image can still be uploaded by hand.</p>
      {c.dongle_list && c.dongle_list.fetched_ago >= 0 && <p class="field-help">Stick list from git: {c.dongle_list.status}.</p>}
    </div>
  );
}

function SupportedDongles() {
  const [list, setList] = useState<DongleModel[] | null>(null);
  const [open, setOpen] = useState(false);
  useEffect(() => { if (open && !list) api<DongleModel[]>("/api/dongles").then(setList).catch(() => setList([])); }, [open]);
  return (
    <Card title={<><Icon name="coordinator" size={18} /> Supported sticks</>} class="span-all"
      actions={<button type="button" class="link-btn" onClick={() => setOpen(!open)}>{open ? "Hide" : "Show list"}</button>}>
      <p class="muted small" style={{ marginTop: 0 }}>Recognised by USB ID and name. Any CP210x, CH340, FTDI or USB CDC serial adapter is driven even when it isn't listed; its protocol is detected from the traffic.
        Firmware can be flashed on the sticks marked below; the others run as bridges only.</p>
      {open && (list ? <div class="table-wrap"><table class="data">
        <thead><tr><th>Stick</th><th>Chip</th><th>USB</th><th>Protocol</th><th>Flash here</th></tr></thead>
        <tbody>{list.map((m) => (
          <tr><td>{m.name}{m.downloaded ? <span class="chip info" style={{ marginLeft: "6px" }}>from git</span> : null}{m.note ? <div class="muted small">{m.note}</div> : null}</td><td>{m.radio}</td>
            <td class="mono small">{m.usb_id} {m.usb_chip}</td><td>{m.protocol}{m.rtscts_wired ? <span class="muted small"> · RTS/CTS</span> : null}</td>
            <td>{m.flashable ? <span class="ok-text">Yes</span> : <span class="muted">No</span>}</td></tr>))}</tbody>
      </table></div> : <Empty>Loading…</Empty>)}
    </Card>
  );
}

interface Heap { free: number; min: number; largest: number; total: number }
interface TaskRow { name: string; stack: number; min_free: number; where: "internal" | "psram"; system?: boolean }
interface TasksState { tasks: TaskRow[]; memory: { internal: Heap; dma: Heap; psram: Heap } }

/** Where the RAM goes (2.0.1+): every task's stack and how close it has come to full, and the three heaps. */
function MemoryCard() {
  const [d, setD] = useState<TasksState | null>(null);
  const [gone, setGone] = useState(false);
  usePoll(async () => {
    try { setD(await api<TasksState>("/api/tasks")); } catch (e: any) { if (e?.status === 404) setGone(true); }
  }, 5000);
  if (gone) return null;  // firmware before 2.0.1
  if (!d) return <Card title="Memory and tasks" class="span-all"><Empty>Loading…</Empty></Card>;
  const heaps: [string, Heap, string][] = [
    ["Internal RAM", d.memory.internal, "Task stacks that write flash, drivers, network and USB"],
    ["DMA-capable", d.memory.dma, "Part of internal RAM: Ethernet and USB transfer buffers"],
    ["PSRAM", d.memory.psram, "Radio task stacks, streams, history, TLS, network buffers"],
  ];
  const rows = [...d.tasks].sort((a, b) => Number(!!a.system) - Number(!!b.system) || a.name.localeCompare(b.name));
  const inPsram = d.tasks.filter((t) => t.where === "psram").reduce((s, t) => s + t.stack, 0);
  const internal = d.tasks.filter((t) => t.where === "internal").reduce((s, t) => s + t.stack, 0);
  return (
    <Card title={<><Icon name="chip" size={18} /> Memory and tasks</>} class="span-all"
      actions={<span class="muted small">stacks: {bytes(internal)} internal · {bytes(inPsram)} in PSRAM</span>}>
      <div class="table-wrap"><table class="data">
        <thead><tr><th>Heap</th><th class="num">Free</th><th class="num">Lowest</th><th class="num">Largest block</th><th class="num">Size</th></tr></thead>
        <tbody>{heaps.map(([n, h, what]) => (
          <tr><td>{n}<div class="muted small">{what}</div></td><td class="num">{bytes(h.free)}</td><td class="num">{bytes(h.min)}</td>
            <td class="num">{bytes(h.largest)}</td><td class="num">{bytes(h.total)}</td></tr>))}</tbody>
      </table></div>
      <div class="table-wrap" style={{ marginTop: "12px" }}><table class="data">
        <thead><tr><th>Task</th><th>Stack in</th><th class="num">Stack</th><th class="num">Most used</th><th class="num">Least free</th><th style={{ width: "22%" }}>Use</th></tr></thead>
        <tbody>{rows.map((t) => {
          const used = Math.max(0, t.stack - t.min_free), pct = t.stack ? Math.min(100, (used * 100) / t.stack) : 0;
          const tight = t.min_free < 512;
          return (
            <tr><td class="mono">{t.name}{t.system ? <span class="muted small"> · system</span> : null}</td>
              <td>{t.where === "psram" ? <span class="chip info">PSRAM</span> : <span class="muted">internal</span>}</td>
              <td class="num">{bytes(t.stack)}</td><td class="num">{bytes(used)}</td>
              <td class={`num${tight ? " warn-text" : ""}`}>{bytes(t.min_free)}</td>
              <td><div class="meter"><i style={{ width: `${pct}%`, background: tight ? "var(--warn)" : undefined }} /></div></td></tr>
          );
        })}</tbody>
      </table></div>
      <p class="field-help">"Least free" is the closest each stack has come to full since boot. A task whose stack is in PSRAM never writes
        flash itself; the ones that save settings, logs or firmware keep internal stacks.</p>
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
        <UsbCard />
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
            <div><button type="button" class="btn" onClick={rebootNow}><Icon name="power" size={15} /> Reboot</button><span class="muted small">About 10 s without the sticks.</span></div>
            <div><button type="button" class="btn" onClick={resetStats}><Icon name="refresh" size={15} /> Reset statistics</button></div>
            <div><button type="button" class="btn danger-text" onClick={factory}><Icon name="trash" size={15} /> Factory reset…</button><span class="muted small">Same as 5 quick power cycles (under 10 s each).</span></div>
          </div>
        </Card>
        <MemoryCard />
      </div>
      <Log />
    </div>
  );
}
