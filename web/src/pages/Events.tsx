// The bridge's event log: connections, sticks, joins and leaves, route errors, resets. With several radios: all of
// them, or one picked radio's events (with or without the bridge-wide ones).

import { useEffect, useRef, useState } from "preact/hooks";
import { atUptime, events, extraRadios, radioAt, radioPick } from "../store";
import { dayClock, matches } from "../format";
import { Empty, Search, Seg } from "../ui";
import { Icon } from "../icons";
import { RadioSwitcher } from "./Radios";
import type { BridgeEvent } from "../types";

const ICON = ["info", "alert", "alert"];
const radioOf = (e: BridgeEvent) => (typeof e.radio === "number" ? e.radio : -1);

export function Events() {
  const [lvl, setLvl] = useState<"0" | "1" | "2">("0");
  const [q, setQ] = useState("");
  const [bridgeToo, setBridgeToo] = useState<"yes" | "no">("yes");
  const seen = useRef(0);
  const all = events.value;
  const multi = extraRadios.value.length > 0;
  const pick = radioPick.value;
  const inScope = (e: BridgeEvent) => pick === null || radioOf(e) === pick || (bridgeToo === "yes" && radioOf(e) < 0);
  const scoped = all.filter(inScope);
  const list = scoped.filter((e) => e.level >= Number(lvl) && matches(q, e.text)).slice().reverse();
  const fresh = seen.current;
  useEffect(() => { seen.current = all.length ? all[all.length - 1].id : 0; });
  const warn = scoped.filter((e) => e.level === 1).length, err = scoped.filter((e) => e.level === 2).length;
  const tag = (e: BridgeEvent) => {
    const i = radioOf(e);
    if (!multi || i < 0 || pick !== null) return null;
    const r = radioAt(i);
    return <span class="ev-radio" title={`Radio ${i + 1}${r ? ` · ${r.name}` : ""}`}>{i + 1}{r ? ` · ${r.name}` : ""}</span>;
  };
  return (
    <div class="page">
      <div class="page-head">
        <div>
          <h1>Events</h1>
          <p class="lede">{scoped.length} events{pick !== null ? ` for radio ${pick + 1}${bridgeToo === "yes" ? " and the bridge" : ""}` : ""}{err ? ` · ${err} errors` : ""}{warn ? ` · ${warn} warnings` : ""} since boot (the bridge keeps the last 160)</p>
        </div>
        <div class="toolbar">
          <Seg label="Level" value={lvl} onChange={setLvl} options={[{ v: "0", label: "All" }, { v: "1", label: "Warnings" }, { v: "2", label: "Errors" }]} />
          <Search value={q} onInput={setQ} placeholder="Filter events" />
        </div>
      </div>
      {multi && (
        <div class="toolbar radio-switch">
          <RadioSwitcher />
          {pick !== null && <Seg label="Bridge events" value={bridgeToo} onChange={setBridgeToo}
            options={[{ v: "yes", label: "With bridge events" }, { v: "no", label: "This radio only" }]} />}
        </div>
      )}
      <div class="card">
        {list.length === 0 ? <Empty>Nothing here.</Empty> : (
          <ul class="ev-list">
            {list.map((e) => (
              <li class={`l${e.level} ${fresh && e.id > fresh ? "fresh" : ""}`} key={e.id}>
                <Icon name={ICON[e.level]} size={16} />
                <time title={new Date((e.epoch || atUptime(e.up)) * 1000).toLocaleString()}>{dayClock(e.epoch || atUptime(e.up))}</time>
                <span class="msg">{tag(e)}{e.text}</span>
              </li>
            ))}
          </ul>
        )}
      </div>
    </div>
  );
}
