// Build the Win32 ACL verifier bundled with the Windows desktop client.
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const tauri = fileURLToPath(new URL("../src-tauri/", import.meta.url));
const output = join(tauri, "generated-runtime", "windows-acl.exe");
const compiler = process.env.PET_MANAGER_WINDOWS_ACL_CXX || "cl.exe";
const cross = process.platform !== "win32";
if (cross && !process.env.PET_MANAGER_WINDOWS_ACL_CXX) {
  throw new Error("Set PET_MANAGER_WINDOWS_ACL_CXX and the Windows SDK compiler environment for cross builds.");
}
mkdirSync(dirname(output), { recursive: true });
const temporary = mkdtempSync(join(dirname(output), ".windows-acl-"));
try {
  copyFileSync(join(tauri, "installer", "windows", "windows-acl.cpp"), join(temporary, "windows-acl.cpp"));
  const result = spawnSync(compiler, [
    ...(cross ? ["--target=x86_64-pc-windows-msvc", "-fuse-ld=lld"] : []),
    "/nologo", "/std:c++20", "/MT", "/O2", "/EHsc", "/W4", "/WX", "/utf-8",
    "/Brepro", "/D_WIN32_WINNT=0x0A00",
    "windows-acl.cpp", "/Fe:windows-acl.exe",
    "/link", "/Brepro", "advapi32.lib", "bcrypt.lib",
  ], { cwd: temporary, stdio: "inherit", windowsHide: true });
  if (result.error || result.status !== 0) {
    throw new Error("Windows ACL verifier compilation failed. Use an x64 MSVC developer environment.");
  }
  const executable = join(temporary, "windows-acl.exe");
  const bytes = readFileSync(executable);
  const peOffset = bytes.length >= 64 ? bytes.readUInt32LE(0x3c) : bytes.length;
  if (bytes.toString("ascii", 0, 2) !== "MZ" || peOffset + 6 > bytes.length
      || bytes.readUInt32LE(peOffset) !== 0x4550 || bytes.readUInt16LE(peOffset + 4) !== 0x8664) {
    throw new Error("Windows ACL verifier must be an x64 PE executable.");
  }
  renameSync(executable, output);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
