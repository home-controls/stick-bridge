// Formatting: the kit's shared wording, plus the bridge's own.

export { ago, clock, dayClock, duration, bytes, matches, count as num } from "./kit/format";

const SL_STATUS: Record<number, string> = {
  0x0004: "Busy", 0x0040: "No MAC ack", 0x004c: "Channel busy (CCA)", 0x0054: "TX scheduler", 0x0058: "TX blocked",
  0x0c02: "Delivery failed", 0x0c03: "Message limit", 0x0c13: "Source route failed", 0x0c14: "Many-to-one route failed",
  0x0c20: "No APS ack", 0x0c21: "APS cancelled", 0x0c24: "Route discovery", 0x0c25: "Unicast failed",
  0x0c26: "No route", 0x0c27: "Sleepy child timeout", 0x0c28: "Broadcast relay failed",
};
export function slStatus(code: number): string {
  return SL_STATUS[code] || `0x${code.toString(16).padStart(4, "0")}`;
}
