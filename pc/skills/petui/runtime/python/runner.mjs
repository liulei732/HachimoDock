// This process runs one PetUI snapshot with no WebAssembly host imports.
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const MAX_REQUEST = 112 * 1024;
const MAX_SOURCE = 16 * 1024;
const MAX_INPUT = 32 * 1024;
const MAX_OUTPUT = 16 * 1024;
const RUNTIME_SHA256 = 'f457188d0616ff1827f5b1b00a3b94d2f6854ea612c7a8919dcaa0c9239335e2';

try {
  const chunks = [];
  let total = 0;
  for await (const chunk of process.stdin) {
    total += chunk.length;
    if (total > MAX_REQUEST) throw new Error('request too large');
    chunks.push(chunk);
  }
  const decoder = new TextDecoder('utf-8', { fatal: true });
  const request = JSON.parse(decoder.decode(Buffer.concat(chunks)));
  if (typeof request.source !== 'string') throw new Error('invalid source');
  const source = Buffer.from(request.source);
  const input = Buffer.from(JSON.stringify(request.input));
  if (!source.length || source.length >= MAX_SOURCE || input.length >= MAX_INPUT) {
    throw new Error('input too large');
  }

  const binary = readFileSync(new URL('./python.wasm', import.meta.url));
  if (createHash('sha256').update(binary).digest('hex') !== RUNTIME_SHA256) throw new Error('runtime hash mismatch');
  const module = new WebAssembly.Module(binary);
  if (WebAssembly.Module.imports(module).length) throw new Error('runtime requires host imports');
  const vm = new WebAssembly.Instance(module, {}).exports;
  vm._initialize();
  const memory = new Uint8Array(vm.memory.buffer);
  memory.set(source, vm.petui_source());
  memory.set(input, vm.petui_input());
  const length = vm.petui_run(source.length, input.length);
  if (length <= 0 || length >= MAX_OUTPUT) throw new Error('invalid plugin result');
  const result = decoder.decode(memory.subarray(vm.petui_output(), vm.petui_output() + length));
  const data = JSON.parse(result);
  if (!data || Array.isArray(data) || typeof data !== 'object') throw new Error('expected object');
  // Do not print exceptions, plugin source, or the data it receives.
  process.stdout.write(JSON.stringify(data));
} catch {
  process.exitCode = 1;
}
