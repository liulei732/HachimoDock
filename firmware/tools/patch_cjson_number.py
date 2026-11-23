"""Use the cJSON reader's decimal conversion for the printer's precision check."""
import argparse
import hashlib
from pathlib import Path

PINNED_IDF_CJSON = "298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d"


def patch(source):
    if hashlib.sha256(source).hexdigest() != PINNED_IDF_CJSON:
        raise ValueError("Review the updated ESP-IDF cJSON number printer")
    original = b'        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))'
    replacement = b"""        char *end;
        test = strtod((const char*)number_buffer, &end);
        if ((end == (char*)number_buffer) || !compare_double(test, d))"""
    if source.count(original) != 1:
        raise ValueError("Expected exactly one floating-point readback")
    return source.replace(original, replacement)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = patch(args.input.read_bytes())
    if not args.output.exists() or args.output.read_bytes() != result:
        args.output.write_bytes(result)
