// Devices the coordinator heard, from the traffic passing through: signal, link quality, delivery. Only radio 1's
// EmberZNet traffic is decoded, so with several radios the other radios get a note and their own counters instead.

import { useState } from "preact/hooks";
import { devices, atUptime, extraRadios, radioAt, radioPick, status } from "../store";
import { RadioStatsCard, RadioSwitcher, ROLE_LABEL } from "./Radios";
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
  const multi = extraRadios.value.length > 0;
  const pick = radioPick.value;
  const r0 = radioAt(0);
  const r0zigbee = (r0?.role ?? status.value?.radios?.[0]?.role ?? "zigbee") === "zigbee";

  // Another radio picked: its devices live in its own app; show its counters so the page isn't empty
  const other = multi && pick !== null && pick !== 0 ? radioAt(pick) : undefined;
  if (other) {
    const app = other.role === "zha" ? "ZHA in Home Assistant" : other.role === "thread" ? "the border router (and the Thread apps on top of it)"
      : other.role === "zwave" ? "Z-Wave JS" : "the Zigbee2MQTT that uses it";
    return (
      <div class="page">
        <div class="page-head"><div><h1>Devices heard</h1><p class="lede">Radio {other.index + 1} · {other.name}</p></div></div>
        <RadioSwitcher />
        <p class="note"><Icon name="info" size={16} /> The bridge reads devices from radio 1's Zigbee2MQTT traffic only. Devices on {other.name}{other.name !== ROLE_LABEL[other.role] ? ` (${ROLE_LABEL[other.role]})` : ""} are in {app}.</p>
        <div class="grid two"><RadioStatsCard r={other} /></div>
      </div>
    );
  }

  return (
    <div class="page">
      <div class="page-head">
        <div>
          <h1>Devices heard</h1>
          <p class="lede">{multi ? `Radio 1 · ${r0?.name ?? "Zigbee"}: ` : ""}{num(devices.value.length)} devices talked through this coordinator{weak ? ` · ${weak} with a weak last hop` : ""}{failing ? ` · ${failing} failing deliveries` : ""}</p>
        </div>
        <div class="toolbar"><Search value={q} onInput={setQ} placeholder="Filter devices" /></div>
      </div>
      <RadioSwitcher />
      {multi && <p class="note"><Icon name="info" size={16} /> These devices come from radio 1 ({r0?.name ?? "Zigbee"}): the bridge decodes only its EmberZNet (Zigbee2MQTT) traffic. The other radios' devices are in their own apps.</p>}
      {!r0zigbee && <p class="note"><Icon name="alert" size={16} /> Radio 1's role isn't Zigbee2MQTT: this table only fills from EmberZNet traffic.</p>}
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
