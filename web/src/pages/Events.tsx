// The bridge's event log: connections, dongle, joins and leaves, route errors, resets.

import { useEffect, useRef, useState } from "preact/hooks";
import { atUptime, events } from "../store";
import { dayClock, matches } from "../format";
import { Empty, Search, Seg } from "../ui";
import { Icon } from "../icons";

const ICON = ["info", "alert", "alert"];

export function Events() {
  const [lvl, setLvl] = useState<"0" | "1" | "2">("0");
  const [q, setQ] = useState("");
  const seen = useRef(0);
  const all = events.value;
  const list = all.filter((e) => e.level >= Number(lvl) && matches(q, e.text)).slice().reverse();
  const fresh = seen.current;
  useEffect(() => { seen.current = all.length ? all[all.length - 1].id : 0; });
  const warn = all.filter((e) => e.level === 1).length, err = all.filter((e) => e.level === 2).length;
  return (
    <div class="page">
      <div class="page-head">
        <div>
          <h1>Events</h1>
          <p class="lede">{all.length} events{err ? ` · ${err} errors` : ""}{warn ? ` · ${warn} warnings` : ""} since boot (the bridge keeps the last 160)</p>
        </div>
        <div class="toolbar">
          <Seg label="Level" value={lvl} onChange={setLvl} options={[{ v: "0", label: "All" }, { v: "1", label: "Warnings" }, { v: "2", label: "Errors" }]} />
          <Search value={q} onInput={setQ} placeholder="Filter events" />
        </div>
      </div>
      <div class="card">
        {list.length === 0 ? <Empty>Nothing here.</Empty> : (
          <ul class="ev-list">
            {list.map((e) => (
              <li class={`l${e.level} ${fresh && e.id > fresh ? "fresh" : ""}`} key={e.id}>
                <Icon name={ICON[e.level]} size={16} />
                <time title={new Date((e.epoch || atUptime(e.up)) * 1000).toLocaleString()}>{dayClock(e.epoch || atUptime(e.up))}</time>
                <span class="msg">{e.text}</span>
              </li>
            ))}
          </ul>
        )}
      </div>
    </div>
  );
}
