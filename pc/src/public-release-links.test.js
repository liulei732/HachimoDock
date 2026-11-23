import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const read = name => readFileSync(new URL("../../" + name, import.meta.url), "utf8");
const release = JSON.parse(read("latest-release.json"));
const repository = "https://github.com/ai-maker-lab/HachimoDock";

test("public download instructions reference published artifacts in both languages", () => {
  assert.equal(release.releaseTag, "v" + release.desktopVersion);
  assert.equal(release.releaseUrl, repository + "/releases/tag/" + release.releaseTag);
  for (const page of ["README.md", "README.en.md"]) {
    const text = read(page);
    for (const role of ["macos", "windows"]) {
      const artifact = release.artifacts.find(asset => asset.role === role);
      assert.ok(artifact, role);
      assert.ok(text.includes(repository + "/releases/download/" + release.releaseTag + "/" + artifact.file), page + " " + role);
    }
    assert.ok(text.includes(release.firmwareVersion) && text.includes(release.releaseUrl), page);
    assert.ok(text.includes("(latest-release.json)"), page);
    for (const [, tag, file] of text.matchAll(/\/releases\/download\/([^/)]+)\/([^\s)]+)/g)) {
      assert.equal(tag, release.releaseTag);
      assert.ok(release.artifacts.some(asset => asset.file === file), file);
    }
  }
});
