import test from "node:test";
import assert from "node:assert/strict";
import { broadcastLink, discoveryBoard } from "./probe-component-wlan.mjs";

const row = {family:"IPv4",internal:false,address:"192.168.16.34",netmask:"255.255.252.0"};
test("only the selected attached IPv4 broadcast is eligible", () => {
  assert.equal(broadcastLink([row]).broadcast,"192.168.19.255");
  for (const rows of [[], [row,row], [{...row,internal:true}], [{...row,netmask:"255.255.255.254"}],
    [{...row,netmask:"255.1.3.0"}], [{...row,netmask:"0.0.0.0"}]]) assert.throws(() => broadcastLink(rows));
});
test("only matching bounded product replies from the local subnet count", () => {
  const link=broadcastLink([row]), peer={address:"192.168.19.1",port:29462};
  const reply={nonce:"fresh",protocol:2,id:"p4-112233445566"};
  const receive = (body,from=peer) => discoveryBoard(Buffer.from(JSON.stringify(body)),from,link,"fresh");
  assert.equal(receive(reply),reply.id);
  for (const data of [null,{...reply,nonce:"old"},{...reply,id:"other"},{...reply,protocol:1},{...reply,pad:"x".repeat(513)}]) assert.equal(receive(data),null);
  for (const other of [{...peer,port:80},{...peer,address:"192.168.20.1"},{...peer,address:link.broadcast}]) assert.equal(receive(reply,other),null);
});
