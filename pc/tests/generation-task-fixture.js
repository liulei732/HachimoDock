// Compile the actual ESM declarations (including re-exports and aliased imports)
// and replace only dependency modules in this isolated state-machine fixture.
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { transformSync } from "esbuild";
import * as generationIssues from "../src/lib/generation-issue.js";

const source = readFileSync(new URL("../src/lib/generation-task.js", import.meta.url), "utf8");
const injectedModules = new Set([
  "./avatar-pipeline/run.js", "./avatar-pipeline/families.js",
  "./appearance-store.js", "./avatar-pipeline/diagnostics.js",
  "./avatar-pipeline/session.js", "./avatar-pipeline/provider-config.js",
]);

export function loadGenerationTaskFixture(context, expose) {
  context.module = { exports: {} };
  context.require = name => {
    if (name === "./generation-issue.js") return generationIssues;
    if (!injectedModules.has(name)) throw Error("Unexpected generation dependency: " + name);
    return context;
  };
  const { code } = transformSync(source + "\n" + expose, { format: "cjs", target: "es2022" });
  vm.runInContext(code, context);
  return context.api;
}
