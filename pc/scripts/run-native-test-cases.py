"""Run independent Windows libtest cases with bounded time and a complete failure list."""
import concurrent.futures
import os
from pathlib import Path
import subprocess
import sys

BINARY = str(Path(sys.argv[1]).resolve(strict=True))


def list_tests(*options):
    output = subprocess.check_output([BINARY, "--list", "--format", "terse", *options], text=True)
    return [line.removesuffix(": test") for line in output.splitlines() if line.endswith(": test")]


def run_case(name):
    process = subprocess.Popen(
        [BINARY, "--exact", name, "--test-threads", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
    )
    try:
        output, _ = process.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        process.kill()
        process.communicate()
        return name, "exceeded 120 seconds"
    if process.returncode or "1 passed; 0 failed" not in output:
        return name, output[-3500:]
    return name, None


if __name__ == "__main__":
    # GitHub redirects stdout on Windows. Diagnostics include localized messages.
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    cases = list_tests()
    ignored = set(list_tests("--ignored"))
    enabled = [name for name in cases if name not in ignored]
    if not enabled or len(cases) != len(set(cases)) or not ignored.issubset(cases):
        sys.exit("Unexpected Rust test inventory")
    print(f"Running {len(enabled)} cases, {len(ignored)} explicitly ignored", flush=True)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as pool:
        futures = [pool.submit(run_case, name) for name in enabled]
        for count, future in enumerate(concurrent.futures.as_completed(futures), 1):
            name, error = future.result()
            if error:
                failed.append(name)
                print(f"FAILED {name}\n{error}", flush=True)
            if count % 100 == 0:
                print(f"Completed {count}/{len(enabled)} cases", flush=True)
    print(f"Windows native: {len(enabled) - len(failed)} passed, {len(failed)} failed, {len(ignored)} ignored")
    sys.exit(bool(failed))
