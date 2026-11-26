// The shell: the Zigbee UI kit's (logo + name, tabs on top or at the bottom on a phone, status pills,
// one banner, one dialog, toasts), with the bridge's pills and banners.

import { useEffect } from "preact/hooks";
import { ROLE_CLIENT, auth, awaitReboot, post, reachable, rebootNeeded, refreshAll, setRadioPick, startPolling, status, toast } from "./store";
import { Icon } from "./icons";
import { href, route } from "./route";
import { confirmIt } from "./dialog";
import { Pill, Shell, Splash } from "./kit/shell";
import { Toasts } from "./kit/toast";
import { initTheme } from "./kit/theme";
import { Auth } from "./pages/Auth";
import { Overview } from "./pages/Overview";
import { Devices } from "./pages/Devices";
import { Events } from "./pages/Events";
import { Settings } from "./pages/Settings";
import { radioProblem, radioState, stickName } from "./pages/Radios";
import { System } from "./pages/System";

const NAV = [
  { id: "overview", label: "Overview", icon: "health" },
  { id: "devices", label: "Devices", icon: "list" },
  { id: "events", label: "Events", icon: "logs" },
  { id: "settings", label: "Settings", icon: "gear" },
  { id: "system", label: "System", icon: "terminal" },
];

initTheme("zb.theme");

export async function rebootNow() {
  if (!(await confirmIt({ title: "Reboot the bridge?", body: "Every radio's client (Zigbee2MQTT and the others) loses its stick for about 10 seconds and reconnects by itself.", confirm: "Reboot" }))) return;
  try {
    await post("/api/reboot");
    awaitReboot("Rebooting…");
  } catch (e: any) {
    toast(e.message, "error");
  }
}

export function App() {
  const a = auth.value;
  useEffect(() => {
    if (a?.authed) {
      void refreshAll();
      startPolling();
    }
  }, [a?.authed]);

  if (!a) {
    return <Splash>{reachable.value ? "Connecting…" : "Can't reach the bridge, retrying…"}</Splash>;
  }
  if (!a.authed) return <><Auth state={a} /><Toasts /></>;

  const page = route.value[0];
  const s = status.value;
  return (
    <Shell app="bridge" version={s?.fw.version} nav={NAV} page={NAV.some((n) => n.id === page) ? page : "overview"} right={<TopRight />} banner={<Banners />}>
      {page === "devices" ? <Devices /> : page === "events" ? <Events /> : page === "settings" ? <Settings />
        : page === "system" ? <System /> : <Overview />}
    </Shell>
  );
}

const PILL_LABEL: Record<string, string> = { zigbee: "Zigbee2MQTT", zha: "ZHA", thread: "Thread", zwave: "Z-Wave JS" };

function TopRight() {
  const s = status.value;
  if (!s) return null;
  const net = s.ncp.network;
  const radios = s.radios ?? [];
  return (
    <>
      <Pill ok={s.eth.link && s.eth.has_ip} label={s.eth.link ? `${s.eth.speed} Mbps` : "No link"} title={s.eth.link ? `Ethernet ${s.eth.speed} Mbps, ${s.eth.ip}` : "Ethernet link down"} />
      {/* One pill per radio, named after its app: its colour covers the stick, the client and (radio 1) the Zigbee network.
          A click opens that radio on the Overview. */}
      {radios.map((r) => {
        const bad = radioProblem(r);
        const st = radioState(r);
        const netDown = r.index === 0 && net === "down";
        const tone = netDown ? "bad" : st.tone === "muted" ? undefined : st.tone;
        const parts = [
          `Radio ${r.index + 1}${r.name ? ` (${r.name})` : ""}`,
          bad ?? (r.state === "no_stick" ? "no stick" : r.state === "opening" ? "opening the stick" : r.state === "flashing" ? "the stick is being flashed"
            : `${stickName(r)}${r.stick.hub_port ? ` on hub port ${r.stick.hub_port}` : ""}`),
          r.client.connected ? `${ROLE_CLIENT[r.role] ?? "client"} connected from ${r.client.ip}` : `waiting for ${ROLE_CLIENT[r.role] ?? "a client"} on tcp/${r.tcp_port}`,
          ...(r.index === 0 && net !== "unknown" ? [`Zigbee network ${net}`] : []),
        ];
        return (
          <span key={r.index} onClick={() => setRadioPick(r.index)}>
            <Pill tone={tone} label={PILL_LABEL[r.role] ?? r.name} title={parts.join(" · ")} href={href("overview")} />
          </span>
        );
      })}
      {s.update_available && (
        <Pill tone="warn" icon="download" href={href("system")} label={`Update ${s.update_available}`} title={`Firmware ${s.update_available} is available on git: install it on the System page`} />
      )}
    </>
  );
}

function Banners() {
  if (!reachable.value) return <div class="banner warn"><Icon name="refresh" size={16} /> The bridge isn't answering; retrying… showing the last known state.</div>;
  const s = status.value;
  if (s && !s.eth.link) return <div class="banner bad"><Icon name="alert" size={16} /> Ethernet link is down.</div>;
  if (rebootNeeded.value) {
    return (
      <div class="banner warn"><Icon name="refresh" size={16} /> Saved. Network and full-speed-only USB changes take effect after a reboot.
        <button type="button" class="link-btn" onClick={rebootNow}>Reboot now</button></div>
    );
  }
  if (s?.fw.pending_verify) return <div class="banner warn"><Icon name="info" size={16} /> New firmware {s.fw.version} is on trial; it's kept after 30 s of running.</div>;
  return null;
}

