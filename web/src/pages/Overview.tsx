// Everything at a glance: health, live Zigbee traffic, coordinator response time, the two
// connections (Zigbee2MQTT and the dongle) and the serial link.

import { RANGES, atUptime, chartRange, delivery, history, historyLong, lastN, rttLastMinute, setChartRange, status, type ChartRange } from "../store";
import { LineChart, type XY } from "../chart";
import { bytes, duration, num } from "../format";
import { Card, Chip, Empty, KV, Stat } from "../ui";
import { Icon } from "../icons";
import { href } from "../route";
import type { Flow, History, Line } from "../types";

// Points grouped into buckets aligned to the bridge's clock (uptime multiples of `bucket` seconds), so a
// finished bucket never changes when the window slides; the newest, still-filling bucket is left out
// (drawn half-empty it would make every line dip at the right edge). `step` = seconds per sample.
function buckets(h: History, col: keyof History, how: "sum" | "avg" | "max" | "avgNZ", bucket: number, step: number, from: number): XY[] {
  const out: XY[] = [];
  const t = h.t, a = h[col];
  const last = t.length ? t[t.length - 1] : 0;
  const per = Math.max(1, Math.round(bucket / step));  // samples in a full bucket
  let key = -1, sum = 0, n = 0, nz = 0, nzSum = 0, mx = 0;
  const flush = () => {
    if (key < 0 || n === 0) return;
    let v: number | null;
    if (how === "sum") {
      if (n * 2 < per) return;  // mostly empty (window edge, reboot): no point rather than a guess
      v = n < per ? (sum * per) / n : sum;  // a few samples short: scaled to the full bucket
    }
    else if (how === "max") v = mx;
    else if (how === "avg") v = sum / n;
    else v = nz ? nzSum / nz : null;  // no commands in the slot: a gap, not 0
    out.push([atUptime(key), v, null, null]);
  };
  for (let i = 0; i < t.length; i++) {
    if (t[i] < from) continue;
    const k = Math.ceil(t[i] / bucket) * bucket;  // bucket end
    if (k !== key) { flush(); key = k; sum = n = nz = nzSum = mx = 0; }
    const v = a[i];
    sum += v; n++;
    if (v > mx) mx = v;
    if (v > 0) { nz++; nzSum += v; }
  }
  if (key >= 0 && key <= last) flush();  // the last bucket only once its end has passed
  return out;
}

function RangePicker() {
  return (
    <div class="seg small-seg" role="group" aria-label="Chart range">
      {(Object.keys(RANGES) as ChartRange[]).map((r) => (
        <button type="button" aria-pressed={chartRange.value === r} onClick={() => setChartRange(r)}>{RANGES[r].label}</button>
      ))}
    </div>
  );
}

function HealthRing({ score }: { score: number }) {
  const C = 2 * Math.PI * 22;
  const col = score >= 85 ? "var(--ok)" : score >= 60 ? "var(--warn)" : "var(--bad)";
  return (
    <div class="health-ring">
      <svg width="30" height="30" viewBox="0 0 56 56" aria-hidden="true">
        <circle class="ring-track" cx="28" cy="28" r="22" stroke-width="8" fill="none" />
        <circle class="ring-arc" cx="28" cy="28" r="22" stroke-width="8" fill="none" stroke-linecap="round"
          stroke={col} stroke-dasharray={C} stroke-dashoffset={C * (1 - score / 100)} />
      </svg>
      <span style={{ color: col }}>{score}<span class="unit"> / 100</span></span>
    </div>
  );
}

const EDGES = ["< 1 ms", "1–2", "2–5", "5–10", "10–20", "20–50", "50–100", "100–200", "200–500", "0.5–1 s", "1–2 s", "> 2 s"];

export function Overview() {
  const s = status.value;
  const h = history.value;
  if (!s) return <div class="page"><Empty>Loading…</Empty></div>;
  const k = s.counters;
  // ASH/EZSP tables only mean something for EmberZNet dongles
  const ezsp = !s.dongle.protocol || s.dongle.protocol.startsWith("EmberZNet") || s.dongle.protocol === "unknown";
  const d = delivery.value;
  const rtt = rttLastMinute.value;
  const rxMin = lastN("rx", 60);
  const range = RANGES[chartRange.value];
  const long = chartRange.value !== "15m";
  const hc = long ? historyLong.value : h;  // what the charts draw
  const step = long ? 10 : 1;
  const B = range.bucket;
  const lastUp = h?.t.length ? h.t[h.t.length - 1] : hc?.t.length ? hc.t[hc.t.length - 1] : 0;
  const x1 = lastUp ? atUptime(lastUp) : Date.now() / 1000;
  const x0 = x1 - range.secs; // a fixed window that fills from the right
  const from = lastUp - range.secs;
  const bk = (col: keyof History, how: "sum" | "avg" | "max" | "avgNZ") => (hc ? buckets(hc, col, how, B, step, from) : []);
  const per = B >= 60 ? `${B / 60} min` : `${B} s`;
  const stack = s.ncp.stack ? `EmberZNet ${s.ncp.stack}` : "Coordinator not identified yet";

  return (
    <div class="page">
      <div class="page-head">
        <div>
          <h1>Bridge</h1>
          <p class="lede">{stack}{s.ncp.ezsp ? ` · EZSP ${s.ncp.ezsp}` : ""} · tcp://{s.eth.ip}:{s.client.tcp_port}</p>
        </div>
        <div class="head-right"><RangePicker /><span class="live"><span class="dot" /> Live</span></div>
      </div>

      <div class="stats five">
        <div class={`stat ${s.health.score >= 85 ? "ok" : s.health.score >= 60 ? "warn" : "bad"}`}>
          <div class="stat-label">Health</div>
          <div class="stat-value"><HealthRing score={s.health.score} /></div>
          <div class="stat-sub">{s.health.reason}</div>
        </div>
        <Stat label="Zigbee2MQTT" value={s.client.connected ? "Connected" : "Waiting"} tone={s.client.connected ? "ok" : "bad"}
          sub={s.client.connected ? `${s.client.ip} · ${duration(s.uptime - s.client.since)}` : `listening on tcp/${s.client.tcp_port}`} />
        <Stat label="Messages in" value={<>{num(rxMin)}<span class="unit"> /min</span></>}
          sub={`${num(k.zb_rx)} in the last ${duration(s.uptime - s.stats_since)}`} />
        <Stat label="Delivery" value={d.pct === null ? "—" : `${d.pct >= 99.95 ? "100" : d.pct.toFixed(1)} %`}
          tone={d.pct === null ? "" : d.pct >= 98 ? "ok" : d.pct >= 90 ? "warn" : "bad"} sub={`${num(d.ok)} ok · ${num(d.fail)} failed (5 min)`} />
        <Stat label="Response time" value={rtt.avg === null ? "—" : <>{rtt.avg.toFixed(1)}<span class="unit"> ms</span></>}
          sub={rtt.avg === null ? "no commands in the last minute" : `slowest ${Math.round(rtt.max)} ms (1 min)`} />
      </div>

      <div class="grid two stretch">
        <Card title={<><Icon name="signal" size={18} /> Zigbee traffic</>} actions={<span class="muted small">messages per {per}</span>}>
          {hc && hc.t.length > 1 ? (
            <LineChart zero label="Zigbee messages" x0={x0} x1={x1} height={190} step={B} series={[
              { name: "Received", color: "var(--series-1)", points: bk("rx", "sum") },
              { name: "Delivered", color: "var(--series-3)", points: bk("ok", "sum") },
              { name: "Failed", color: "var(--bad)", points: bk("fail", "sum") },
            ]} />
          ) : <Empty>{long && !historyLong.value ? "Loading…" : "Collecting…"}</Empty>}
        </Card>
        <Card title={<><Icon name="health" size={18} /> Coordinator response time</>}
          actions={<span class="muted small" title="From Zigbee2MQTT's command to the coordinator's answer, measured here. The shaded band reaches the slowest answer in each slot.">command to answer · shaded = slowest</span>}>
          {hc && hc.t.length > 1 ? (
            <LineChart zero label="Response time" unit="ms" x0={x0} x1={x1} height={190} step={B} series={[
              { name: "Average", color: "var(--series-2)", points: (() => {
                const mx = bk("rttmax", "max");
                return bk("rtt", "avgNZ").map((p, i) => (p[1] === null ? [p[0], null, null, null] : [p[0], p[1], p[1], mx[i]?.[1] ?? p[1]]) as XY);
              })() },
            ]} refs={[{ y: 50, color: "var(--warn)", label: "50 ms" }]} />
          ) : <Empty>{long && !historyLong.value ? "Loading…" : "Collecting…"}</Empty>}
        </Card>
      </div>

      <div class="masonry" style={{ marginTop: "16px" }}>
        <Card title={<><Icon name="link" size={18} /> Zigbee2MQTT</>} actions={s.client.connected ? <Chip tone="ok">Connected</Chip> : <Chip tone="bad">Not connected</Chip>}>
          <KV rows={[
            ["From", s.client.connected ? `${s.client.ip}:${s.client.port}` : "—"],
            ["Connected for", s.client.connected ? duration(s.uptime - s.client.since) : "—"],
            ["Allowed host", s.client.allowed || "anyone"],
            ["Listening on", `tcp/${s.client.tcp_port}`],
            ["Sessions", `${num(k.clients_accepted)} accepted · ${num(k.clients_refused)} refused · ${num(k.client_drops)} dropped`],
          ]} />
        </Card>
        <Card title={<><Icon name="coordinator" size={18} /> Coordinator</>} actions={s.dongle.open ? <Chip tone="ok">Connected</Chip> : <Chip tone="bad">No dongle</Chip>}>
          <KV rows={[
            ["Dongle", s.dongle.open ? s.dongle.model_name && !/^Unknown/.test(s.dongle.model_name) ? s.dongle.model_name : s.dongle.product || s.dongle.model_name || "USB serial" : "Plug it into J3"],
            ...(s.dongle.open && s.dongle.radio ? [["Radio", `${s.dongle.radio} · ${s.dongle.family}`] as [string, any]] : []),
            ["Serial", s.dongle.serial ? <span class="mono">{s.dongle.serial}</span> : "—"],
            ["USB", s.dongle.open ? `${s.dongle.usb_id}${s.dongle.usb_chip ? ` · ${s.dongle.usb_chip}` : ""} · ${num(s.dongle.baud)} baud` : "—"],
            ...(s.dongle.open && s.dongle.protocol ? [["Protocol", <>{s.dongle.protocol}{s.dongle.z2m_adapter ? <span class="muted"> · Z2M adapter <span class="mono">{s.dongle.z2m_adapter}</span></span> : null}{!s.dongle.protocol_detected ? <span class="muted"> (expected)</span> : null}</>] as [string, any]] : []),
            ...(s.dongle.open && s.dongle.note ? [["Note", <span class="muted">{s.dongle.note}</span>] as [string, any]] : []),
            ["Firmware", s.dongle.fw_running ? s.dongle.fw_running : s.ncp.stack ? `${stack}${s.ncp.ezsp ? ` · EZSP ${s.ncp.ezsp}` : ""}` : "—"],
            ["Zigbee network", s.ncp.network === "up" ? <span class="ok-text">Up</span> : s.ncp.network === "down" ? <span class="bad-text">Down</span> : <span class="muted">Unknown until Zigbee2MQTT starts it</span>],
            ["Connected for", s.dongle.open ? duration(s.uptime - s.dongle.since) : "—"],
          ]} />
        </Card>
        <Card title={<><Icon name="upload" size={18} /> Serial link</>} actions={<span class="muted small">USB ⇄ TCP</span>}>
          {hc && hc.t.length > 1 ? (
            <LineChart zero label="Bytes per second" unit="B/s" x0={x0} x1={x1} height={130} step={B} series={[
              { name: "From dongle", color: "var(--series-1)", points: bk("in", "avg") },
              { name: "To dongle", color: "var(--series-2)", points: bk("out", "avg") },
            ]} />
          ) : <Empty>Collecting…</Empty>}
          <KV rows={[
            ["Moved", `${bytes(k.ncp.bytes)} in · ${bytes(k.host.bytes)} out`],
            ["Link errors", (() => {
              const e = k.host.crc_err + k.ncp.crc_err + k.host.nak + k.ncp.nak + k.ncp.retx + k.host.retx + k.ncp.error;
              return e ? <span class="warn-text">{num(e)}</span> : <span class="ok-text">none</span>;
            })()],
            ["Ack time (EZSP reply)", k.ack_avg_ms ? `${k.ack_avg_ms.toFixed(1)} ms avg · ${Math.round(k.ack_max_ms)} ms max` : "—"],
          ]} />
        </Card>
        {ezsp && <Card title="Response time spread" actions={<span class="muted small">{num(k.rtt_n)} answers, ms</span>}>
          {k.rtt_n ? (
            <div class="buckets">
              {k.rtt_hist.map((v, i) => (
                <div class="bucket">
                  <span class="label">{EDGES[i]}</span>
                  <div class="meter"><i style={{ width: `${(v / Math.max(1, ...k.rtt_hist)) * 100}%` }} /></div>
                  <span class="n">{num(v)}</span>
                </div>
              ))}
            </div>
          ) : <Empty>No commands yet.</Empty>}
        </Card>}
        {ezsp && <Card title="Serial protocol (ASH)">
          <div class="table-wrap">
            <table class="data">
              <thead><tr><th /><th class="num">Z2M → dongle</th><th class="num">Dongle → Z2M</th></tr></thead>
              <tbody>
                {([
                  ["Frames", k.host.frames, k.ncp.frames, false],
                  ["Data", k.host.data, k.ncp.data, false],
                  ["Acks", k.host.ack, k.ncp.ack, false],
                  ["Retransmits", k.host.retx, k.ncp.retx, true],
                  ["NAKs", k.host.nak, k.ncp.nak, true],
                  ["CRC errors", k.host.crc_err, k.ncp.crc_err, true],
                  ["Cancelled", k.host.cancel, k.ncp.cancel, true],
                  ["Resets", k.host.rst, k.ncp.rstack, false],
                  ["ASH errors", 0, k.ncp.error, true],
                ] as [string, number, number, boolean][]).map(([n, a, b, bad]) => (
                  <tr><td>{n}</td><td class={`num ${bad && a ? "warn-text" : ""}`}>{num(a)}</td><td class={`num ${bad && b ? "warn-text" : ""}`}>{num(b)}</td></tr>
                ))}
              </tbody>
            </table>
          </div>
        </Card>}
        {k.flow && <FlowCard f={k.flow} l={k.line} />}
        <Card title="Zigbee">
          <KV rows={[
            ["Sent", `${num(k.zb_unicast)} unicast · ${num(k.zb_broadcast)} broadcast · ${num(k.zb_multicast)} multicast`],
            ["Delivered / failed", `${num(k.zb_sent_ok)} / ${num(k.zb_sent_fail)}`],
            ["Route errors", num(k.route_errors)],
            ["Joins / leaves", `${num(k.joins)} / ${num(k.leaves)}`],
            ["Address conflicts", num(k.id_conflicts)],
          ]} />
          <div class="card-foot"><a href={href("devices")} class="link-btn">Devices heard <Icon name="chevron" size={14} /></a><a href={href("events")} class="link-btn">Events <Icon name="chevron" size={14} /></a></div>
        </Card>
      </div>
    </div>
  );
}

function FlowCard({ f, l }: { f: Flow; l?: Line }) {
  const warn = (v: number) => (v ? <span class="warn-text">{num(v)}</span> : num(v));
  const uartErr = l ? l.err_framing + l.err_hw_overrun + l.err_queue_overrun + l.err_parity + l.err_break : 0;
  const lineRows: [string, any][] = l && l.known ? [
    ["CTS from dongle", <span class={l.cts_now ? "" : "warn-text"}>{`${l.cts_now ? "on" : "off"} · ${num(l.cts_drops)} drops · off ${l.samples ? ((l.cts_off / l.samples) * 100).toFixed(2) : "0"}% of samples${l.hw_flow ? " · honoured" : ""}`}</span>],
    ["UART errors (CP2102N)", uartErr
      ? <span class="warn-text">{[["framing", l.err_framing], ["overrun", l.err_hw_overrun], ["queue overrun", l.err_queue_overrun], ["parity", l.err_parity], ["break", l.err_break]].filter(([, v]) => v).map(([n, v]) => `${num(v as number)} ${n}`).join(" · ")}</span>
      : "none"],
    ["CP2102N queues max", <span class={l.hold_cts ? "warn-text" : ""}>{`${num(l.out_queue_max)} B to dongle · ${num(l.in_queue_max)} B from dongle${l.hold_cts ? ` · held for CTS ${num(l.hold_cts)}×` : ""}`}</span>],
  ] : [];
  return (
    <Card title="Flow control" actions={f.xoff_now ? <Chip tone="warn">Dongle says XOFF</Chip> : <span class="muted small">XON/XOFF · USB OUT</span>}>
      <KV rows={[
        ["XOFF / XON from dongle", <>{warn(f.xoff_rx)} / {num(f.xon_rx)}</>],
        ["Writes held by XOFF", f.xoff_pauses
          ? <>{warn(f.xoff_pauses)} · {f.xoff_pause_avg_ms.toFixed(1)} ms avg · {f.xoff_pause_max_ms.toFixed(1)} ms max{f.xoff_timeouts ? <> · <span class="warn-text">{num(f.xoff_timeouts)} without XON</span></> : null}</>
          : f.xoff_honor ? "none" : "not held (counted only)"],
        ["USB OUT", `${num(f.chunks)} chunks for ${num(f.writes)} writes${f.write_errors ? ` · ${num(f.write_errors)} failed` : ""}`],
        ["Waited for the line", f.paced ? `${num(f.paced)} chunks · ${num(Math.round(f.pace_avg_us))} µs avg · ${num(f.pace_max_us)} µs max` : "none"],
        ["Queue high-water", `${num(f.queue_hwm)} bytes`],
        ["Longest write", `${f.write_max_ms.toFixed(1)} ms`],
        ...lineRows,
        ["Pacing", f.chunk ? `${f.chunk}-byte chunks · ${num(f.gap_us)} µs gap · XOFF ${f.xoff_honor ? `held up to ${f.xoff_max_ms} ms` : "not held"}` : "off (whole writes)"],
      ]} />
    </Card>
  );
}
