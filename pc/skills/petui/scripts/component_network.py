"""Provider-independent declaration for the component-network-v1 WASM host."""
import json
import re
PROFILE = "component-network-v1"
CREDENTIAL_PROFILE = "component-credentials-secure-v1"

def validate(policy):
    def obj(value, allowed):
        if not isinstance(value, dict) or set(value) - set(allowed):
            raise ValueError("Invalid component network fields")
    def domain(value):
        if (not isinstance(value, str) or len(value) > 95 or "." not in value
            or not re.search("[a-z]", value)
            or any(not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", p) for p in value.split("."))):
            raise ValueError("Expected an explicit lowercase DNS host")
    obj(policy, ("profile", "https", "mqtts", "credentials"))
    if policy.get("profile") != PROFILE:
        raise ValueError("Unsupported component network profile")
    if len(json.dumps(policy, ensure_ascii=False, separators=(",", ":")).encode()) >= 2048:
        raise ValueError("Network policy exceeds 2047 bytes")
    https, mqtt = policy.get("https", []), policy.get("mqtts", [])
    if not isinstance(https, list) or len(https) > 4 or not isinstance(mqtt, list) or len(mqtt) > 2:
        raise ValueError("Network origin/broker limit")
    seen = set()
    for origin in https:
        obj(origin, ("origin", "methods"))
        url = origin.get("origin")
        if not isinstance(url, str) or not url.startswith("https://"):
            raise ValueError("HTTPS origin required")
        domain(url[8:])
        methods = origin.get("methods")
        if (url in seen or not isinstance(methods, list) or not methods
            or any(m not in ("GET", "POST") for m in methods) or len(set(methods)) != len(methods)):
            raise ValueError("Invalid methods or duplicate origin")
        seen.add(url)
    seen.clear()
    for broker in mqtt:
        obj(broker, ("host", "port", "subscribe", "publish"))
        domain(broker.get("host"))
        if broker.get("host") in seen or type(broker.get("port")) is not int or broker["port"] != 8883:
            raise ValueError("Explicit MQTT TLS broker on port 8883 required")
        seen.add(broker["host"])
        for key in ("subscribe", "publish"):
            filters = broker.get(key)
            if not isinstance(filters, list) or len(filters) > 8:
                raise ValueError("MQTT topic filter limit")
            for value in filters:
                if not isinstance(value, str) or not 1 <= len(value.encode()) <= 128 or any(ord(c) < 32 or ord(c) == 127 for c in value):
                    raise ValueError("Invalid topic filter")
                parts = value.split("/")
                if any(("+" in p and p != "+") or ("#" in p and (p != "#" or i != len(parts)-1)) for i, p in enumerate(parts)):
                    raise ValueError("Malformed MQTT wildcard")
    credentials = policy.get("credentials")
    if "credentials" in policy:
        obj(credentials, ("records", "totalBytes"))
        if any(type(credentials.get(k)) is not int or not 1 <= credentials[k] <= maximum for k, maximum in (("records", 8), ("totalBytes", 8192))):
            raise ValueError("Credential storage quota")
    if not https and not mqtt and credentials is None:
        raise ValueError("Empty network policy")
    return policy
