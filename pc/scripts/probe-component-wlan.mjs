// Read-only product discovery on a single operator-selected local interface.
import { randomBytes } from "node:crypto";
import dgram from "node:dgram";
import { isIPv4 } from "node:net";
import { networkInterfaces } from "node:os";
import { pathToFileURL } from "node:url";
import { setTimeout as delay } from "node:timers/promises";

const port = 29462;
const boardPattern = /^p4-[0-9a-f]{12}$/;

function bits(address) {
  return address.split(".").reduce((n, byte) => n * 256 + Number(byte), 0);
}

export function broadcastLink(rows) {
  const v4 = rows?.filter(r => !r.internal && r.family === "IPv4");
  if (v4?.length !== 1) throw new Error("Select an interface with one active non-loopback IPv4 address.");
  const {address, netmask} = v4[0];
  if (!isIPv4(address) || !isIPv4(netmask)) throw new Error("Invalid interface address.");
  const mask = bits(netmask);
  if (!/^1{1,30}0+$/.test(mask.toString(2).padStart(32,"0"))) throw new Error("The interface has no usable broadcast subnet.");
  const network = (bits(address) & mask) >>> 0;
  const end = network + (0xffffffff - mask);
  const broadcast = [24,16,8,0].map(shift => (end >>> shift) & 255).join(".");
  return {address, network, end, broadcast};
}

export function discoveryBoard(packet, peer, link, nonce) {
  if (packet.length > 512 || peer.port !== port || !isIPv4(peer.address)) return null;
  const ip = bits(peer.address);
  if (ip <= link.network || ip >= link.end) return null;
  try {
    const reply = JSON.parse(packet.toString("utf8"));
    if (reply?.protocol === 2 && reply.nonce === nonce && typeof reply.id === "string"
        && boardPattern.test(reply.id)) return reply.id;
  } catch { /* Ignore unrelated datagrams. */ }
  return null;
}

async function probe(iface, board) {
  const link = broadcastLink(networkInterfaces()[iface]);
  const nonce = randomBytes(16).toString("hex");
  const request = Buffer.from("HACHI-DISCOVER-1 " + nonce);
  const responders = new Set();
  const socket = dgram.createSocket("udp4");
  const lifetime = new AbortController();
  let sent = 0;
  try {
    const failure = new Promise((_, reject) => socket.once("error", reject));
    const collect = async () => {
      await new Promise(resolve => socket.bind(0, link.address, resolve));
      socket.setBroadcast(true);
      socket.on("message", (packet, peer) => {
        const id = discoveryBoard(packet, peer, link, nonce);
        if (id) responders.add(id);
      });
      for (let i = 0; i < 8; ++i) {
        await new Promise((resolve, reject) => socket.send(request, port, link.broadcast, err => err ? reject(err) : resolve()));
        ++sent;
        await delay(750, undefined, {signal:lifetime.signal});
      }
    };
    await Promise.race([failure, collect()]);
    return {interface:iface, durationSeconds:6, datagramsSent:sent, distinctBoardReplies:responders.size,
      ...(board ? {targetReplied:responders.has(board)} : {})};
  } finally {
    lifetime.abort();
    socket.close();
  }
}

if (process.argv[1] && pathToFileURL(process.argv[1]).href === import.meta.url) {
  const [iface, board, ...extra] = process.argv.slice(2);
  if (!iface || iface === "--help" || extra.length || (board !== undefined && !boardPattern.test(board))) {
    console.log("Usage: node pc/scripts/probe-component-wlan.mjs <Wi-Fi interface> [p4-board-id]\nSends product discovery on the current subnet for six seconds. A reply identifies a candidate for paired diagnostics.");
    if (iface !== "--help") process.exitCode = 2;
  } else {
    try { console.log(JSON.stringify(await probe(iface, board), null, 2)); }
    catch (err) { console.error(err.code || err.message); process.exitCode = 1; }
  }
}
