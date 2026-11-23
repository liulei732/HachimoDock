"""Optional integration test with the actual SDK compiler, WAMR and browser VM.

Set HACHI_CLANG, HACHI_WASM_LD, HACHI_WASM_CHECK and HACHI_NODE to local tools.
The native checker must be built from this checkout, not an installed old release.
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]

def test_network_wasm_abi(tmp_path):
    names = ("HACHI_CLANG", "HACHI_WASM_LD", "HACHI_WASM_CHECK", "HACHI_NODE")
    if not all(os.environ.get(name) for name in names):
        pytest.skip("local WASM compiler/checker and Node paths are not configured")
    wasm = tmp_path / "network.wasm"
    scripts = ROOT / "pc/skills/petui/scripts"
    subprocess.run([os.sys.executable, str(scripts / "compile_wasm.py"),
        str(ROOT / "firmware/tests/component_network_wasm.c"),
        str(ROOT / "pc/skills/petui/sdk"), os.environ[names[0]], os.environ[names[1]], str(wasm)], check=True)
    subprocess.run([os.environ[names[2]], str(wasm)], check=True, timeout=30)
    subprocess.run([os.environ[names[3]], "--input-type=module", "-e", '''
import fs from 'node:fs';
import { createRuntime } from './pc/src/component-center/wasm-preview/runtime.mjs';
const run=await createRuntime(fs.readFileSync(process.argv[1]));
for(const [kind,args] of [['init',[]],['event',[1,0,0]],['frame',[33]]]) {
 if(run(kind,args).networkProfile!=='component-network-v1') throw Error('network profile missing');
}
''', str(wasm)], cwd=ROOT, check=True, timeout=30)
    # Exercise the same import allowlist used by the package builder.
    import sys
    sys.path.insert(0, str(scripts))
    try:
        spec = importlib.util.spec_from_file_location("pack_hachiapp_network_test", scripts / "pack_hachiapp.py")
        pack = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pack)
        assert pack.wasm_profile(wasm.read_bytes(), include_network=True)[3]
    finally:
        sys.path.remove(str(scripts))
