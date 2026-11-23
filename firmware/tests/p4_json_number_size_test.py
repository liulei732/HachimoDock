"""Compare the complete SDK printer output before and after the WiFi6 size change."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

FW = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(FW / "tools"))
from patch_cjson_number import patch


def test_unreviewed_json_source_is_rejected():
    with pytest.raises(ValueError, match="Review"):
        patch(b"unreviewed")


def test_number_print_and_parse_match_pinned_sdk(tmp_path):
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", FW.parents[1] / "toolchains/platformio/v1"))
    source = core / "packages/framework-espidf/components/json/cJSON/cJSON.c"
    if not source.is_file():
        pytest.skip("requires the pinned local ESP-IDF cJSON source")
    optimized = tmp_path / "optimized.c"
    optimized.write_bytes(patch(source.read_bytes()))
    driver = tmp_path / "check.c"
    driver.write_text(r"""
#include "cJSON.h"
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void check(double d) {
  cJSON item = {.type = cJSON_Number, .valuedouble = d};
  if (isfinite(d))
    item.valueint = d >= INT_MAX ? INT_MAX : d <= INT_MIN ? INT_MIN : (int)d;
  char *out = cJSON_PrintUnformatted(&item);
  assert(out && strlen(out) < 26);
  puts(out);
  cJSON *parsed = cJSON_Parse(out);
  assert(parsed && (isfinite(d) ? cJSON_IsNumber(parsed) : cJSON_IsNull(parsed)));
  if (isfinite(d)) assert(fwrite(&parsed->valuedouble, sizeof(double), 1, stdout) == 1);
  cJSON_Delete(parsed);
  char field[32];
  for (int space = (int)strlen(out); space <= (int)strlen(out)+2; ++space) {
    int ok = cJSON_PrintPreallocated(&item,field,space,0);
    assert(!ok || strcmp(field,out)==0);
    putchar(ok ? 'Y' : 'N');
  }
  cJSON_free(out);
}
int main(int argc, char **argv) {
  assert(argc == 2 && setlocale(LC_NUMERIC, argv[1]));
  double edges[] = {0., -0., INFINITY, -INFINITY, NAN, DBL_MIN, DBL_MAX,
                    DBL_TRUE_MIN, -DBL_TRUE_MIN, DBL_EPSILON, INT_MAX, INT_MIN,
                    INT_MAX+1., INT_MIN-1., 1./3., -1./3., 1e-6, 1e-7, -1e300};
  for (size_t i=0; i<sizeof(edges)/sizeof(*edges); i++) {
    check(edges[i]); check(nextafter(edges[i], 0.)); check(nextafter(edges[i], INFINITY));
  }
  uint64_t bits=UINT64_C(0x1234abcd5678abcd);
  for (unsigned i=0; i<24000; ++i) {
    bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
    double number;
    memcpy(&number,&bits,sizeof(number)); check(number);
  }
}
""")
    locales = ["C"]
    available = subprocess.check_output(["locale", "-a"], text=True).splitlines()
    decimal_comma = next((s for s in available if s.lower().startswith(("fr_fr.", "de_de."))), None)
    if decimal_comma:
        locales.append(decimal_comma)
    outputs = []
    for name, unit in [("sdk", source), ("patched", optimized)]:
        binary = tmp_path / name
        subprocess.run(["cc", "-std=c11", "-O2", "-DENABLE_LOCALES", "-I", str(source.parent),
                        str(driver), str(unit), "-lm", "-o", str(binary)], check=True)
        outputs.append([subprocess.check_output([binary, loc]) for loc in locales])
    assert outputs[0] == outputs[1]
