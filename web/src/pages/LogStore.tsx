// The stored log (flash, kept across reboots) and its copy on a microSD card: where it lives, how far back it
// goes, downloads, and a viewer for the newest part with search and a level filter.

import { useEffect, useMemo, useState } from "preact/hooks";
import { api, post, toast } from "../store";
import { bytes, duration } from "../format";
import type { LogsInfo } from "../types";
import { Card, Chip, Empty, Seg } from "../ui";
import { Icon } from "../icons";
import { confirmIt } from "../dialog";

const TAIL = 256 * 1024;  // what the viewer loads: the newest ~256 KB (a couple of thousand lines)
const SHOW = 2000;        // lines rendered at most

const stampText = (ms: number) => {
  const d = new Date(ms);
  const today = d.toDateString() === new Date().toDateString();
  const t = d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  return today ? t : `${d.toLocaleDateString(undefined, { month: "short", day: "numeric" })} ${t}`;
};

/** Where the stored log lives, how full it is, the card, downloads. */
export function LogStorage() {
  const [info, setInfo] = useState<LogsInfo | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [allFiles, setAllFiles] = useState(false);
  const load = async () => {
    try { setInfo(await api<LogsInfo>("/api/logs")); setErr(null); } catch (e: any) { setErr(e.message); }
  };
  useEffect(() => {
    void load();
    const t = setInterval(load, 10000);
    return () => clearInterval(t);
  }, []);
  const title = <><Icon name="logs" size={18} /> Log storage</>;
  if (!info) return <Card title={title} class="span-all">{err ? <p class="bad-text small">{err}</p> : <Empty>Loading…</Empty>}</Card>;

  const f = info.flash, sd = info.sd;
  const now = Date.now();
  const span = f.oldest ? (now - f.oldest) / 1000 : 0;
  const pct = f.size ? Math.min(100, ((f.used ?? 0) / f.size) * 100) : 0;
  // How long the area lasts at the rate so far (only once there's a day of history to go on).
  const lasts = f.size && f.used && span > 86400 ? (f.size / f.used) * span : 0;
  const clear = async () => {
    if (!(await confirmIt({ title: "Clear the stored log?", confirm: "Clear", danger: true,
      body: "Everything stored in flash so far is hidden and its space reused. The live log and any files on the SD card stay." }))) return;
    try { await post("/api/logs/clear"); toast("Stored log cleared"); void load(); } catch (e: any) { toast(e.message, "error"); }
  };
  const files = sd.files ?? [];
  const shown = allFiles ? files : files.slice(0, 8);
  const sdPct = sd.total ? Math.min(100, ((sd.total - (sd.free ?? 0)) / sd.total) * 100) : 0;
  return (
    <Card title={title} class="span-all" actions={<span class="muted small">boot {f.boot}{f.clock_wait ? " · waiting for the clock" : ""}</span>}>
      <div class="logstore">
        <div>
          <div class="update-head"><b>Flash</b> <span class="muted">· {f.where}</span></div>
          {f.ok ? <>
            <div class="meter"><i style={{ width: `${pct}%`, background: "var(--accent)" }} /></div>
            <p class="field-help">
              {bytes(f.used ?? 0)} of {bytes(f.size ?? 0)} used
              {f.oldest ? <> · goes back to {new Date(f.oldest).toLocaleString()} ({duration(span)})</> : null}
              {lasts ? <> · room for about {duration(lasts)} at this rate, then the oldest lines make way</> : null}
            </p>
            {!!f.write_errors && <p class="bad-text small"><Icon name="alert" size={14} /> {f.write_errors} flash write errors</p>}
          </> : <p class="bad-text small"><Icon name="alert" size={14} /> The stored log isn't running ({f.where}). The live log still works.</p>}
          <p class="field-help">Kept through reboots, updates and power cuts. After a crash or watchdog reset the last lines from memory are saved too. Times are stored in UTC and shown in your time zone.</p>
          <div class="row-btns">
            <a class={`btn${f.ok ? "" : " disabled"}`} href="/api/logs/text?src=flash&download=1" download><Icon name="download" size={15} /> Download all</a>
            <button type="button" class="btn" disabled={!f.ok} onClick={clear}><Icon name="trash" size={15} /> Clear…</button>
          </div>
        </div>
        <div>
          <div class="update-head">
            <b>SD card</b>
            {sd.state === "ok" ? <Chip tone="ok" icon="check">{sd.detail}</Chip>
              : sd.state === "error" ? <Chip tone="bad" icon="alert">Problem</Chip>
              : <Chip tone="muted">No card</Chip>}
          </div>
          {sd.state === "ok" ? <>
            <div class="meter"><i style={{ width: `${sdPct}%`, background: "var(--accent)" }} /></div>
            <p class="field-help">{bytes((sd.total ?? 0) - (sd.free ?? 0))} of {bytes(sd.total ?? 0)} used · {sd.file_count} day files, {bytes(sd.file_bytes ?? 0)} · card {sd.card}, {Math.round((sd.khz ?? 0) / 1000)} MHz</p>
            {files.length ? <ul class="file-list">
              {shown.map((x) => (
                <li><a href={`/api/logs/text?src=sd&name=${encodeURIComponent(x.name)}&download=1`} download><Icon name="download" size={14} /> {x.name}</a><span class="muted small">{bytes(x.size)}</span></li>
              ))}
              {files.length > 8 && <li><button type="button" class="link-btn" onClick={() => setAllFiles(!allFiles)}>{allFiles ? "Fewer" : `All ${files.length} shown here`}</button></li>}
            </ul> : <p class="field-help">No files yet: the first lines go on the card within a few seconds.</p>}
          </> : <>
            {sd.state === "error" && <p class="bad-text small">{sd.detail}</p>}
            <p class="field-help">Put a microSD card (FAT32) in the slot and the bridge also keeps one file per day on it, <span class="mono">zblog/2026-11-24.log</span>, deleting the oldest when it's nearly full. It's picked up within 30 s{sd.retry_in ? ` (next look in ${sd.retry_in} s)` : ""}. Cards over 32 GB usually come as exFAT: reformat them as FAT32.</p>
          </>}
        </div>
      </div>
    </Card>
  );
}

const STAMPED = /^(\S+) ([EWIDV]) \((\d+)\) ([^:]+): (.*)$/;
const NOTE = /^(\S+) # (.*)$/;

interface Line { cls: string; t: string; title: string; lv: string; ns: string; msg: string; note: boolean }

function parseStamp(s: string): { t: string; title: string } {
  if (s.endsWith("Z")) {
    const ms = Date.parse(s);
    if (!Number.isNaN(ms)) return { t: stampText(ms), title: new Date(ms).toLocaleString() };
  }
  return { t: s.replace(/^boot\+/, "+"), title: `${s}: the clock wasn't set yet (time since that boot)` };
}

function parse(text: string): Line[] {
  const out: Line[] = [];
  for (const l of text.split("\n")) {
    if (!l) continue;
    let m = l.match(STAMPED);
    if (m) { const s = parseStamp(m[1]); out.push({ cls: `lv-${m[2].toLowerCase()}`, t: s.t, title: s.title, lv: m[2], ns: m[4], msg: m[5], note: false }); continue; }
    m = l.match(NOTE);
    if (m) { const s = parseStamp(m[1]); out.push({ cls: "log-note", t: s.t, title: s.title, lv: "#", ns: "", msg: m[2], note: true }); continue; }
    const sp = l.indexOf(" ");
    const s = sp > 0 ? parseStamp(l.slice(0, sp)) : { t: "", title: "" };
    out.push({ cls: "", t: s.t, title: s.title, lv: "", ns: "", msg: sp > 0 ? l.slice(sp + 1) : l, note: false });
  }
  return out;
}

/** The newest part of the stored log (previous boots included), searchable. */
export function StoredLog() {
  const [text, setText] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [q, setQ] = useState("");
  const [lv, setLv] = useState<"all" | "warn" | "err" | "boots">("all");
  const load = async () => {
    try {
      const r = await fetch(`/api/logs/text?src=flash&tail=${TAIL}`, { credentials: "same-origin", cache: "no-store" });
      if (!r.ok) throw new Error((await r.text()) || `HTTP ${r.status}`);
      setText(await r.text());
      setErr(null);
    } catch (e: any) { setErr(e.message); }
  };
  useEffect(() => { void load(); }, []);
  const lines = useMemo(() => (text ? parse(text) : []), [text]);
  const needle = q.trim().toLowerCase();
  const pick = lines.filter((l) =>
    (lv === "all" || (lv === "warn" && (l.lv === "W" || l.lv === "E" || l.note)) || (lv === "err" && (l.lv === "E" || l.note)) || (lv === "boots" && l.note)) &&
    (!needle || `${l.ns} ${l.msg}`.toLowerCase().includes(needle)));
  const shown = pick.slice(-SHOW);
  return (
    <>
      <div class="log-tools">
        <label class="search"><Icon name="search" size={15} /><input type="search" placeholder="Search the stored log" value={q} onInput={(e) => setQ((e.target as HTMLInputElement).value)} /></label>
        <Seg label="Levels" value={lv} onChange={setLv} options={[{ v: "all", label: "All" }, { v: "warn", label: "Warnings" }, { v: "err", label: "Errors" }, { v: "boots", label: "Boots" }]} />
        <button type="button" class="btn" onClick={load}><Icon name="refresh" size={15} /> Reload</button>
        <a class="btn" href="/api/logs/text?src=flash&download=1" download><Icon name="download" size={15} /> Download all</a>
      </div>
      <div class="log sys-log">
        {err ? <Empty>{err}</Empty> : text === null ? <Empty>Loading…</Empty> : shown.length === 0 ? <Empty>{lines.length ? "Nothing matches" : "Nothing stored yet"}</Empty> : shown.map((l) => (
          <div class={`log-line ${l.cls}`}>
            <span class="log-ts" title={l.title}>{l.t}</span>
            <span class="log-lv">{l.lv}</span>
            {!l.note && <span class="log-ns">{l.ns}</span>}
            <span class="log-msg">{l.msg}</span>
          </div>
        ))}
      </div>
      <p class="field-help">
        The newest {bytes(TAIL)} of the stored log{pick.length > SHOW ? `, last ${SHOW} of ${pick.length} matching lines` : ""}. Lines marked # are the bridge's own notes: boots, clock set, lines saved after a crash. Download all for everything.
      </p>
    </>
  );
}
