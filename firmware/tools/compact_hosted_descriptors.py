"""Apply protoc-c's CODE_SIZE descriptor representation to pinned generated C.

RPC fields are addressed by tag. The wireless driver also prints enum names
when a synchronous response times out, including during radio initialization.
Keep the vendor's exact wire schema and header, including protobuf-c version,
default values, numeric indices, initializers and struct offsets.
"""
import argparse
import hashlib
from pathlib import Path
import re

INPUT_SHA256 = "ee8cde248a97b4ea6b86f80ec51b46b0fec2cd51583ebf293b6c84cb5ba3a6d3"


def compact(source):
    if hashlib.sha256(source).hexdigest() != INPUT_SHA256:
        raise ValueError("Review protobuf-c CODE_SIZE conversion for the new ESP-Hosted schema")
    text = source.decode("utf-8")
    # Equivalent layouts are in the pinned SDK protoc-c/c_{message,enum,field}.cc.
    text = re.sub(r'^static const (?:unsigned \w+__field_indices_by_name|'
                  r'ProtobufCEnumValueIndex \w+__enum_values_by_name)\[.*?^\};\n',
                  '', text, flags=re.M | re.S)
    text = re.sub(r'(PROTOBUF_C__(?:MESSAGE|ENUM)_DESCRIPTOR_MAGIC,\n)'
                  r'(?:  "[^"\n]*",\n){4}', r'\g<1>  NULL, NULL, NULL, NULL,\n', text)
    text = re.sub(r'^  \w+__field_indices_by_name,$', '  NULL,', text, flags=re.M)
    text = re.sub(r'^  \d+,\n  \w+__enum_values_by_name,$', '  0, NULL,', text, flags=re.M)
    text = re.sub(r'(  \{\n)    "[^"\n]+",\n(    \d+,\n    PROTOBUF_C_LABEL_)',
                  r'\g<1>    NULL,\n\g<2>', text)
    # rpc_core.c::rpc_id_name dereferences the first string after numeric lookup.
    # Preserve printable names for every enum, including future diagnostic paths.
    text = re.sub(r'^  \{ ("[^"]+"), "[^"]+", (-?\d+) \},$',
                  r'  { \1, NULL, \2 },', text, flags=re.M)
    text = re.sub(r'^#define \w+__field_indices_by_name NULL\n', '', text, flags=re.M)
    if '__field_indices_by_name' in text or '__enum_values_by_name' in text:
        raise ValueError("Unexpected name reflection table")
    return text


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(compact(args.source.read_bytes()), encoding="utf-8")
