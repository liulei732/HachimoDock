import test from "node:test";
import assert from "node:assert/strict";
import {
  compareVersions,
  versionState,
  readFavorites,
  updateFavorites,
  publicationStatus,
  publicationCandidates,
  publicationReceiptError,
  COMMUNITY_CATEGORIES,
  COMMUNITY_TABS,
} from "./community-model.js";
test('my components leads community navigation',()=>{
  assert.deepEqual(COMMUNITY_TABS.slice(0,2),[['local','我的组件'],['discover','社区广场']]);
});
test("Hachimi collection is a real publication and discovery category", () => {
  assert.deepEqual(COMMUNITY_CATEGORIES[1], ["hachimi", "哈基米专属"]);
  assert.equal(new Set(COMMUNITY_CATEGORIES.map(([id]) => id)).size, COMMUNITY_CATEGORIES.length);
});
test("community separates published, local and verified device versions", () => {
  assert.equal(compareVersions("1.10.0", "1.9.0"), 1);
  assert.equal(compareVersions("unknown", "1.0.0"), null);
  const state = versionState(
    { id: "x", version: "2.0.0" },
    { version: "1.0.0" },
    { freshness: "live", items: [{ id: "x" }] },
  );
  assert.equal(state.updateAvailable, true);
  assert.equal(state.deviceLabel, "设备 版本未核验");
  assert.equal(versionState({ id: "x" }, null, {}).deviceLabel, "设备未核对");
});
test("favorites retain public metadata only and tolerate corrupt storage", () => {
  const result = updateFavorites([], {
    id: "demo",
    name: "test",
    path: "/private",
    token: "secret",
  });
  assert.equal(result[0].path, undefined);
  assert.equal(result[0].token, undefined);
  assert.deepEqual(updateFavorites(result, result[0]), []);
  assert.deepEqual(readFavorites({ getItem: () => "not json" }), []);
  assert.deepEqual(readFavorites({ getItem: () => '[{"id":"../bad"}]' }), []);
});
test("pending and rejected changes are not shown as published", () => {
  assert.equal(
    publicationStatus({ status: "pending", published: { available: true } }),
    "待审核",
  );
  assert.equal(publicationStatus({ status: "rejected" }), "需修改");
});

test("current catalog favorites remain visible without rewriting storage", () => {
  const saved='[{"id":"city-radio","name":"cached package"}]';
  let writes=0;const storage={getItem:()=>saved,setItem:()=>writes++};
  assert.deepEqual(readFavorites(storage).map(item=>item.id),["city-radio"]);
  assert.equal(writes,0);
});

test("official community versions can replace bundled copies at the same version", () => {
  const work = { id: "city-radio", version: "1.4.1", official: true, contentHash: "a".repeat(64) };
  const bundled = { version: "1.4.1", versionHash: "b".repeat(16), isBundledCopy: true };
  assert.equal(versionState(work, bundled).updateAvailable, true);
  assert.equal(versionState(work, bundled).contentConflict, false);
  assert.equal(versionState(work, { ...bundled, isBundledCopy: false }).contentConflict, true);
  assert.equal(versionState({ ...work, official: false }, bundled).contentConflict, true);
  assert.equal(versionState(work, { ...bundled, version: "1.4.2" }).localNewer, true);
});

test("publication uses formal sources and provenance even for an unknown local capability", () => {
  const local = {id: "custom-mix", version: "1.0.1", libraryPath: "/test/a", isLocal: true,
    valid: false, syncRequirements: {blockedReason: "unknown local capability"}};
  const variant = {...local, libraryPath: "/test/b"};
  const fork = id => ({...local, id, libraryPath: "/test/" + id});
  const ownedUpdate = fork("my-update");
  const items = [local, {sourceAlternatives: [{...local, isLocal:false}, variant]},
    fork("reserved"), fork("official-copy"), fork("hachimo-copy"), fork("downloaded"),
    ownedUpdate, fork("upcoming-todos"), {...fork("bundled"), isBundledCopy:true},
    {...fork("inventory"), isLocal:false}, {...fork("draft"), libraryPath:""}];
  const state = {reservedIds:["reserved"], downloaded:{downloaded:{}, "my-update":{}}};
  const owned = {"my-update": {}};
  assert.deepEqual(publicationCandidates(items,state,owned), [local, variant, ownedUpdate]);
  assert.deepEqual(publicationCandidates(items,null,owned), []);
});

test("only an affirmative server receipt for the selected identity can enable submission", () => {
  const receipt = {id:"my-work", version:"1.0.4", contentHash:"a".repeat(64), files:["component.json"],
    canPublish:true, serverChecked:true};
  const check = change => publicationReceiptError({...receipt, ...change}, "my-work", "1.0.4");
  assert.equal(check({}), "");
  for (const serverChecked of [undefined, null, false, 1, "true"])
    assert.equal(check({serverChecked}), "尚未取得社区预检通过回执，请重新检查。");
  assert.equal(check({canPublish:false,warning:"版本必须递增"}), "版本必须递增");
  for (const change of [{canPublish:undefined},
    {id:"other"}, {version:"1.0.3"}, {contentHash:"short"}, {files:[]}]) assert.ok(check(change));
});
