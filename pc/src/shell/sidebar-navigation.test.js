import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const app = readFileSync(new URL("../App.jsx", import.meta.url), "utf8");
test("bound sidebar omits duplicated summaries and first-time users can start binding", () => {
  const sidebar = app.slice(app.indexOf("<aside"), app.indexOf("</aside>"));
  assert.doesNotMatch(sidebar, /ContextRail|进行中或未保存/);
  assert.match(sidebar, /!activeBoardDeviceId &&/);
  assert.match(sidebar, /onClick=\{\(\) => setView\("setup"\)\}>\s*<Plus size=\{14\} \/>绑定设备/);
});
