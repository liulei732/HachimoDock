// Exact public license/notice resources shared by Windows installer staging.
// No dependency installation, executable invocation or user settings access.
import { createHash } from 'node:crypto';
import { existsSync, lstatSync, mkdirSync, readFileSync, renameSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

export const dependencyLicenseResources = Object.freeze([
  ['Xiaomi-Miloco-LICENSE.md', '376dd562aa127c36a75f39887b1a1cd7c47859f21f097e8a8f1b7bab0ea79a27'],
  ['Espressif-ESP-SR-LICENSE.txt', '923e6274ea64b226c4f0cb9fc29bda915ae343fd6a8fc290ff5e0daaa85f8813'],
  ['sysinfo-LICENSE.txt', '666ccdb468b5fe0f408f90ebb563c95e4d453e89bd1660e93aa7463f2807ae36'],
  ['MiSans-Font-License-Agreement.pdf', '4a93a27cd2bd81b3b5ecfd0a853144a876fa26938a93a68443c67d74172fcb86'],
  ['MiSans-Embedded-Fonts-NOTICE.txt', 'e806b7242bb729e65a97fca2d24865ddc064a48e005b8c981faeeb056c5762c1'],
  ['Espressif-Waveshare-BSP-LICENSE.txt', 'cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30'],
  ['unescaper-0.1.8-MIT-LICENSE.txt', '6e0bad263cbd2aa6235d78e3c26bcfa2668cf32f6b274ae21ce0dd620114c7b3'],
  ['serialport-4.9.0-LICENSE-NOTICE.txt', '00a4d69750686b8fcd2369b17176b4389d67f275fd22b9eab6c66155b72bd4d2'],
  ['MPL-2.0.txt', '3f3d9e0024b1921b067d6f7f88deb4a60cbe7a78e76c64e3f1d7fc3b779b9d04'],
  ['DEPENDENCY-LICENSE-NOTICE.md', '357b4e159c983730eb6fda2b312226f52b511793a223189c8e991f79396c6782'],
  ['Node-26.13.1-LICENSE.txt', '8e603aa87c70d36e497004f0dec63111511cb61b7d65ace12e10ea791599286c'],
  ['DESKTOP-RUNTIME-LICENSES.txt', '96112f163a22976dc821c7328b01582c74870003921c251615c67dd643971662'],
  ['DESKTOP-RUNTIME-DEPENDENCIES.json', '8398c7a14dda03223497de18828befb4f1492a0373afc5faaa0806098daf2388'],
].map(([name, sha256]) => Object.freeze({ name, sha256 })));

const digest = bytes => createHash('sha256').update(bytes).digest('hex');
function directory(path) {
  const st = lstatSync(path);
  if (!st.isDirectory() || st.isSymbolicLink()) throw new Error('Physical license directory required');
}
function verified(path, expected, name) {
  const st = lstatSync(path);
  if (!st.isFile() || st.isSymbolicLink()) throw new Error(`Regular license file required: ${name}`);
  const bytes = readFileSync(path);
  if (digest(bytes) !== expected) throw new Error(`License integrity mismatch: ${name}`);
  return bytes;
}

export function copyDependencyLicenses(sourceDirectory, destinationDirectory) {
  directory(sourceDirectory);
  verifyDesktopLicenseInventory(join(sourceDirectory, ".."));
  // Authenticate every input before creating or writing the output directory.
  const inputs = dependencyLicenseResources.map(item => ({
    ...item, bytes: verified(join(sourceDirectory, item.name), item.sha256, item.name),
  }));
  if (existsSync(destinationDirectory)) directory(destinationDirectory);
  for (const item of inputs) {
    const target = join(destinationDirectory, item.name);
    if (existsSync(target)) verified(target, item.sha256, item.name);
  }
  mkdirSync(destinationDirectory, { recursive: true });
  const receipt = {};
  for (const item of inputs) {
    const target = join(destinationDirectory, item.name);
    if (!existsSync(target)) writeFileSync(target, item.bytes, { flag: 'wx', mode: 0o644 });
    verified(target, item.sha256, item.name);
    receipt[`licenses/${item.name}`] = item.sha256;
  }
  return receipt;
}

// SHA-256 of the executable in each official Node.js v26.13.1 distribution.
const reviewedNodeBinaries = new Set([
  '266dec4dce3a6cdcf5cdc38722a00cf5ae6a12cc51068d2a227655bbb75751ec', // darwin-arm64
  'e85cb6dd5f916a2aa0d05a5874dcf7c69b1ebd7a5347d7ff912bb4d9339f5e23', // darwin-x64
  'f5109dc868ec38703ad8f305e0aceece717cb60d4889873f434017af4e96cccb', // win-x64
]);

export function verifyNodeRuntime(binaryPath) {
  const st = lstatSync(binaryPath);
  if (!st.isFile() || !reviewedNodeBinaries.has(digest(readFileSync(binaryPath)))) {
    throw new Error('Node vendor build has no reviewed matching license carrier. Use the official Node.js 26.13.1 binary.');
  }
}

export function copyNodeRuntimeLicense(binaryPath, sourceDirectory, destinationDirectory) {
  verifyNodeRuntime(binaryPath);
  directory(sourceDirectory);
  const raw = verified(join(sourceDirectory, 'Node-26.13.1-LICENSE.txt'),
    '8e603aa87c70d36e497004f0dec63111511cb61b7d65ace12e10ea791599286c', 'Node-26.13.1-LICENSE.txt');
  if (existsSync(destinationDirectory)) directory(destinationDirectory);
  const target = join(destinationDirectory, 'node.LICENSE');
  mkdirSync(destinationDirectory, { recursive: true });
  if (existsSync(target)) {
    const previous = lstatSync(target);
    if (!previous.isFile()) throw new Error('Regular node.LICENSE file required');
    const previousDigest = digest(readFileSync(target));
    if (previousDigest === digest(raw)) return previousDigest;
    if (previousDigest !== 'e991d81497a85bb24fc6bffae0a3637a6accd6c6bc5ce1f2c5698bd555cf9d49') {
      throw new Error('Unexpected staged node.LICENSE');
    }
  }
  const staging = mkdtempSync(join(destinationDirectory, '.node-license-'));
  try {
    const temporary = join(staging, 'node.LICENSE');
    writeFileSync(temporary, raw, { flag: 'wx', mode: 0o644 });
    renameSync(temporary, target);
  } finally {
    rmSync(staging, { recursive: true, force: true });
  }
  verified(target, digest(raw), 'node.LICENSE');
  return digest(raw);
}

export function verifyDesktopLicenseInventory(repositoryRoot) {
  const directory = join(repositoryRoot,'licenses');
  const inventory = JSON.parse(verified(join(directory,'DESKTOP-RUNTIME-DEPENDENCIES.json'),dependencyLicenseResources.find(x=>x.name==='DESKTOP-RUNTIME-DEPENDENCIES.json').sha256,'DESKTOP-RUNTIME-DEPENDENCIES.json').toString('utf8'));
  const expected = ['pc/package-lock.json','pc/src-tauri/Cargo.lock','pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json'];
  if (JSON.stringify(Object.keys(inventory.lockfiles).sort()) !== JSON.stringify(expected.sort())) throw new Error('Unexpected dependency lock inventory');
  for (const name of expected) {
    if (digest(readFileSync(join(repositoryRoot,name))) !== inventory.lockfiles[name]) throw new Error('Dependency license inventory is stale');
  }
  const bundle = verified(join(directory,'DESKTOP-RUNTIME-LICENSES.txt'),dependencyLicenseResources.find(x=>x.name==='DESKTOP-RUNTIME-LICENSES.txt').sha256,'DESKTOP-RUNTIME-LICENSES.txt');
  if (digest(bundle) !== inventory.bundleSHA256) throw new Error('Dependency license bundle changed');
  for (const entry of inventory.texts) {
    if (!Number.isSafeInteger(entry.bundleOffset) || !Number.isSafeInteger(entry.bytes) || entry.bundleOffset<0 || entry.bytes<1 || entry.bundleOffset+entry.bytes>bundle.length || digest(bundle.subarray(entry.bundleOffset,entry.bundleOffset+entry.bytes))!==entry.sourceSHA256) throw new Error('Upstream license text boundary changed');
  }
  assertDesktopLicenseCoverage(inventory);
  return {coverageComplete:inventory.coverageComplete, unresolvedLicensePackages:inventory.unresolvedLicensePackages, packages:inventory.packages};
}

export function assertDesktopLicenseCoverage(inventory) {
  if (inventory.coverageComplete !== true || !Array.isArray(inventory.unresolvedLicensePackages) || inventory.unresolvedLicensePackages.length !== 0) throw new Error('Required desktop dependency license grants are incomplete');
  if (!Array.isArray(inventory.requiredPackages) || inventory.requiredPackages.length===0 || !Array.isArray(inventory.texts)) throw new Error('Missing required dependency license membership');
  const required=new Set(inventory.requiredPackages);
  const represented=new Set(inventory.texts.map(item=>item.package));
  if (required.size!==inventory.requiredPackages.length || represented.size!==required.size || inventory.packages!==required.size || [...required].some(id=>!represented.has(id))) throw new Error('Required dependency grant missing even though coverage flag is true');
}
