// Devices the coordinator heard, from the traffic passing through: signal, link quality, delivery.

import { useState } from "preact/hooks";
import { devices, atUptime } from "../store";
import { ago, matches, num, slStatus } from "../format";
import { Chip, Empty, Lqi, Rssi, Search } from "../ui";
import { Icon } from "../icons";
import type { ZbDevice } from "../types";

type Key = "short" | "ieee" | "rx" | "last" | "rssi_avg" | "lqi_avg" | "succ" | "tx_fail" | "route_err";
const COLS: { k: Key; label: string; num?: boolean; hide?: string }[] = [
  { k: "short", label: "Address" },
  { k: "ieee", label: "IEEE", hide: "hide-sm" },
  { k: "rx", label: "Messages", num: true },
  { k: "last", label: "Last heard", num: true },
  { k: "rssi_avg", label: "Signal" },
  { k: "lqi_avg", label: "LQI" },
  { k: "succ", label: "Delivery", num: true },
  { k: "route_err", label: "Route errors", num: true, hide: "hide-md" },
];

const succ = (d: ZbDevice) => (d.tx_ok + d.tx_fail ? d.tx_ok / (d.tx_ok + d.tx_fail) : -1);

export function Devices() {
  const [q, setQ] = useState("");
  const [sort, setSort] = useState<{ k: Key; dir: 1 | -1 }>({ k: "last", dir: -1 });
  const list = devices.value.filter((d) => matches(q, d.short, d.ieee));
  const val = (d: ZbDevice, k: Key): number | string =>
    k === "succ" ? succ(d) : k === "last" ? (d.rx ? d.last : -1) : k === "short" || k === "ieee" ? d[k] : d[k];
  list.sort((a, b) => {
    const x = val(a, sort.k), y = val(b, sort.k);
    return (typeof x === "string" ? x.localeCompare(y as string) : (x as number) - (y as number)) * sort.dir;
  });
  const by = (k: Key) => setSort(sort.k === k ? { k, dir: sort.dir === 1 ? -1 : 1 } : { k, dir: k === "short" || k === "ieee" ? 1 : -1 });
  const weak = devices.value.filter((d) => d.rx && d.rssi_avg < -82).length;
  const failing = devices.value.filter((d) => succ(d) >= 0 && succ(d) < 0.9 && d.tx_ok + d.tx_fail >= 5).length;

  return (
    <div class="page">
      <div class="page-head">
        <div>
          <h1>Devices heard</h1>
          <p class="lede">{num(devices.value.length)} devices talked through this coordinator{weak ? ` · ${weak} with a weak last hop` : ""}{failing ? ` · ${failing} failing deliveries` : ""}</p>
        </div>
        <div class="toolbar"><Search value={q} onInput={setQ} placeholder="Filter devices" /></div>
      </div>
      <p class="note"><Icon name="info" size={16} /> Signal and LQI are the last hop into the coordinator, so a device behind a router shows that router's link. Names live in Zigbee2MQTT and Zigbee Studio.</p>
      {devices.value.length === 0 ? (
        <div class="card"><Empty>No Zigbee traffic yet. Devices appear once Zigbee2MQTT is connected and they send something.</Empty></div>
      ) : (
        <div class="card table-wrap">
          <table class="devices">
            <thead>
              <tr>{COLS.map((c) => (
                <th class={`${c.num ? "num-cell" : ""} ${c.hide || ""}`}>
                  <button type="button" onClick={() => by(c.k)}>{c.label}{sort.k === c.k && <span class="sort">{sort.dir === 1 ? "▲" : "▼"}</span>}</button>
                </th>
              ))}</tr>
            </thead>
            <tbody>
              {list.map((d) => {
                const s = succ(d);
                return (
                  <tr>
                    <td><div class="cell-name"><Icon name="router" size={18} /><span class="mono">{d.short}</span></div></td>
                    <td class="mono muted hide-sm">{d.ieee || "—"}</td>
                    <td class="num-cell">{num(d.rx)}</td>
                    <td class="num-cell" title={d.rx ? new Date(atUptime(d.last) * 1000).toLocaleString() : undefined}>{d.rx ? ago(atUptime(d.last)) : "—"}</td>
                    <td>{d.rx ? <Rssi v={d.rssi_avg} /> : <span class="muted">—</span>}</td>
                    <td>{d.rx ? <Lqi v={d.lqi_avg} /> : <span class="muted">—</span>}</td>
                    <td class="num-cell">
                      {s < 0 ? <span class="muted">—</span> : (
                        <span title={d.tx_fail ? `Last failure: ${slStatus(d.last_fail_status)}, ${ago(atUptime(d.last_fail))}` : undefined}>
                          <Chip tone={s >= 0.98 ? "ok" : s >= 0.9 ? "warn" : "bad"}>{(s * 100).toFixed(s >= 0.999 ? 0 : 1)} %</Chip>
                          <span class="muted small"> {num(d.tx_ok + d.tx_fail)}</span>
                        </span>
                      )}
                    </td>
                    <td class={`num-cell hide-md ${d.route_err ? "warn-text" : "muted"}`}>{d.route_err || "—"}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {list.length === 0 && <Empty>Nothing matches.</Empty>}
        </div>
      )}
    </div>
  );
}
