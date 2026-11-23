"""Generate the WiFi6 public trust configuration. Never include an input as C."""
import argparse
import base64
import ipaddress
import json
import os
from pathlib import Path
import re
import tempfile

KEYS = {"PET_COMMUNITY_HOST", "PET_COMMUNITY_PORT", "PET_COMMUNITY_KEY_B64", "PET_COMMUNITY_CA_PEM"}
SPKI = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d03010703420004")


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate configuration field")
        result[key] = value
    return result


def public_fields(path):
    if not path.is_absolute() or not path.is_file() or path.stat().st_size > 16384:
        raise ValueError("expected an absolute path to a bounded public configuration")
    text = path.read_text(encoding="ascii")
    if path.suffix == ".json":
        fields = json.loads(text, object_pairs_hook=unique)
    else:
        pairs = []
        for line in text.splitlines():
            if not line.strip() or line == "#pragma once":
                continue
            match = re.fullmatch(r"#define (PET_COMMUNITY_[A-Z0-9_]+) (.+)", line)
            if not match:
                raise ValueError("expected one literal public field per line")
            pairs.append((match[1], json.loads(match[2])))
        fields = unique(pairs)
    if not isinstance(fields, dict) or set(fields) - KEYS or not KEYS - {"PET_COMMUNITY_CA_PEM"} <= fields.keys():
        raise ValueError("unknown or missing community configuration field")
    host, port = fields["PET_COMMUNITY_HOST"], fields["PET_COMMUNITY_PORT"]
    if not isinstance(host, str) or len(host) > 253 or not all(
        re.fullmatch(r"[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?", label) for label in host.split(".")
    ):
        raise ValueError("expected a DNS hostname or IPv4 literal")
    if re.fullmatch(r"[0-9.]+", host):
        ipaddress.IPv4Address(host)
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("invalid port")
    public = base64.b64decode(fields["PET_COMMUNITY_KEY_B64"], validate=True)
    if len(public) != 91 or not public.startswith(SPKI):
        raise ValueError("expected a P-256 SPKI public key")
    if "PET_COMMUNITY_CA_PEM" in fields:
        ca = fields["PET_COMMUNITY_CA_PEM"]
        if not isinstance(ca, str) or len(ca) > 8192 or not re.fullmatch(
            r"-----BEGIN CERTIFICATE-----\n(?:[a-zA-Z0-9+/=]+\n)+-----END CERTIFICATE-----\n?", ca
        ):
            raise ValueError("expected a single public PEM certificate")
        base64.b64decode("".join(ca.splitlines()[1:-1]), validate=True)
    return fields


def generate(output, source):
    fields = public_fields(Path(source)) if source else {}
    data = "#pragma once\n" + "".join(
        f"#define {key} {json.dumps(value)}\n" for key, value in sorted(fields.items())
    )
    if output.is_file() and output.read_text() == data:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, delete=False) as f:
        f.write(data)
    os.replace(f.name, output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.output, os.environ.get("PETUI_COMMUNITY_CONFIG_INPUT"))
