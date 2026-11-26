// The shell: the Zigbee UI kit's (logo + name, tabs on top or at the bottom on a phone, status pills,
// one banner, one dialog, toasts), with the bridge's pills and banners.

import { useEffect } from "preact/hooks";
import { auth, awaitReboot, post, reachable, rebootNeeded, refreshAll, startPolling, status, toast } from "./store";
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
  if (!(await confirmIt({ title: "Reboot the bridge?", body: "Zigbee2MQTT loses its coordinator for about 10 seconds and reconnects by itself.", confirm: "Reboot" }))) return;
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

function TopRight() {
  const s = status.value;
  if (!s) return null;
  const net = s.ncp.network;
  return (
    <>
      <Pill ok={s.eth.link && s.eth.has_ip} label={s.eth.link ? `${s.eth.speed} Mbps` : "No link"} title={s.eth.link ? `Ethernet ${s.eth.speed} Mbps, ${s.eth.ip}` : "Ethernet link down"} />
      <Pill ok={s.dongle.open} label={s.dongle.open ? "Dongle" : "No dongle"} title={s.dongle.open ? `${s.dongle.product} (${s.dongle.serial})` : "No Zigbee dongle on J3"} />
      <Pill ok={s.client.connected} label={s.client.connected ? "Zigbee2MQTT" : "No Zigbee2MQTT"} title={s.client.connected ? `Zigbee2MQTT connected from ${s.client.ip}` : `Waiting for Zigbee2MQTT on tcp/${s.client.tcp_port}`} />
      {net !== "unknown" && <Pill ok={net === "up"} label={net === "up" ? "Network up" : "Network down"} title={`The coordinator reports the Zigbee network ${net}`} />}
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
      <div class="banner warn"><Icon name="refresh" size={16} /> Saved. Network, port or baud changes take effect after a reboot.
        <button type="button" class="link-btn" onClick={rebootNow}>Reboot now</button></div>
    );
  }
  if (s?.fw.pending_verify) return <div class="banner warn"><Icon name="info" size={16} /> New firmware {s.fw.version} is on trial; it's kept after 30 s of running.</div>;
  return null;
}

