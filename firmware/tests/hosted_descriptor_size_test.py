"""Compare the numeric wire ABI for every ESP-Hosted RPC field in both builds."""
from pathlib import Path
import os
import re
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from compact_hosted_descriptors import compact


def test_compact_schema_refuses_unreviewed_vendor():
    with pytest.raises(ValueError, match="Review"):
        compact(b"unreviewed")


def test_numeric_descriptors_and_round_trip_match_original(tmp_path):
    transport = Path(os.environ.get("PET_P4_HOSTED_TRANSPORT_SOURCE",
        ROOT / "managed_components/espressif__esp_hosted/host/drivers/transport/transport_drv.c"))
    common = transport.parents[3] / "common"
    proto = common / "proto"
    original = proto / "esp_hosted_rpc.pb-c.c"
    if not original.is_file():
        pytest.skip("requires pinned ESP-Hosted generated schema")
    compacted = tmp_path / "compact.c"
    compacted.write_text(compact(original.read_bytes()))
    names = re.findall(r'^const ProtobufCMessageDescriptor (\w+) =', original.read_text(), re.M)
    enums = re.findall(r'^const ProtobufCEnumDescriptor (\w+) =', original.read_text(), re.M)
    assert (len(names), len(enums)) == (300, 14)
    harness = tmp_path / "probe.c"
    harness.write_text('''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_hosted_rpc.pb-c.h"
static void message(const ProtobufCMessageDescriptor *d) {
  printf("M %zu %u\\n", d->sizeof_message, d->n_fields);
  for (unsigned i=0; i<d->n_fields; i++) {
    const ProtobufCFieldDescriptor *f=&d->fields[i];
    assert(protobuf_c_message_descriptor_get_field(d, f->id)==f);
    printf("%u %d %d %u %u %u\\n", f->id, f->label, f->type,
           f->offset, f->quantifier_offset, f->flags);
  }
}
static void enumeration(const ProtobufCEnumDescriptor *d) {
  printf("E %u\\n", d->n_values);
  for (unsigned i=0; i<d->n_values; i++) {
    assert(protobuf_c_enum_descriptor_get_value(d,d->values[i].value)==&d->values[i]);
    assert(d->values[i].name && d->values[i].name[0]);
    printf("%d %s\\n", d->values[i].value, d->values[i].name);
  }
}
int main(void) {
''' + "".join(f"  message(&{n});\n" for n in names) +
        "".join(f"  enumeration(&{n});\n" for n in enums) + '''
  Rpc req = RPC__INIT;
  req.uid = 457;
  req.msg_id = (RpcId) 300;
  unsigned char b[128];
  size_t size=rpc__get_packed_size(&req);
  assert(size>0 && size<=sizeof(b) && rpc__pack(&req,b)==size);
  Rpc *decoded=rpc__unpack(NULL,size,b);
  assert(decoded && decoded->uid==457 && decoded->msg_id==300);
  rpc__free_unpacked(decoded,NULL);
  for (size_t i=0; i<size; i++) printf("%02x", b[i]);
  return 0;
}
''')
    output = []
    for index, schema in enumerate((original, compacted)):
        binary = tmp_path / f"probe-{index}"
        subprocess.run(["cc", "-std=c11", "-Werror", "-I", str(proto),
                        "-I", str(common / "protobuf-c"), str(harness), str(schema),
                        str(common / "protobuf-c/protobuf-c/protobuf-c.c"), "-o", str(binary)],
                       check=True)
        output.append(subprocess.check_output([str(binary)]))
    assert output[0] == output[1]
