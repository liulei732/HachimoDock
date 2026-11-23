//! Declaration validation shared by package import and local authoring.
use serde_json::Value;
pub(crate) const PROFILE: &str = "component-network-v1";
pub(crate) const CREDENTIAL_PROFILE: &str = "component-credentials-secure-v1";

fn object<'a>(
    v: &'a Value,
    allowed: &[&str],
) -> Result<&'a serde_json::Map<String, Value>, String> {
    let o = v.as_object().ok_or("Expected a network policy object")?;
    if o.keys().any(|k| !allowed.contains(&k.as_str())) {
        return Err("Unknown network policy field".into());
    }
    Ok(o)
}
fn domain(s: &str) -> bool {
    s.len() <= 95
        && s.contains('.')
        && s.bytes().any(|c| c.is_ascii_lowercase())
        && s.split('.').all(|p| {
            !p.is_empty()
                && p.len() <= 63
                && !p.starts_with('-')
                && !p.ends_with('-')
                && p.bytes()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-')
        })
}
fn topic(s: &str) -> bool {
    !s.is_empty() && s.len() <= 128 && !s.bytes().any(|c| c < 32 || c == 127) && {
        let parts: Vec<_> = s.split('/').collect();
        parts.iter().enumerate().all(|(i, p)| {
            (!p.contains('+') || *p == "+")
                && (!p.contains('#') || (*p == "#" && i + 1 == parts.len()))
        })
    }
}
fn array<'a>(v: Option<&'a Value>, max: usize) -> Result<&'a [Value], String> {
    match v {
        None => Ok(&[]),
        Some(v) => v
            .as_array()
            .filter(|a| a.len() <= max)
            .map(Vec::as_slice)
            .ok_or("Network policy list limit".into()),
    }
}
pub(crate) fn validate(v: &Value) -> Result<(), String> {
    let o = object(v, &["profile", "https", "mqtts", "credentials"])?;
    if v["profile"] != PROFILE || serde_json::to_vec(v).map_err(|e| e.to_string())?.len() >= 2048 {
        return Err("Network policy profile or size invalid".into());
    }
    let https = array(o.get("https"), 4)?;
    let mqtt = array(o.get("mqtts"), 2)?;
    let mut seen = std::collections::HashSet::new();
    for endpoint in https {
        object(endpoint, &["origin", "methods"])?;
        let origin = endpoint["origin"].as_str().ok_or("Expected HTTPS origin")?;
        if !origin.strip_prefix("https://").is_some_and(domain) || !seen.insert(origin) {
            return Err("Expected unique explicit HTTPS origin".into());
        }
        let methods = endpoint["methods"]
            .as_array()
            .ok_or("Expected HTTP methods")?;
        let mut bits = 0;
        for m in methods {
            let bit = match m.as_str() {
                Some("GET") => 1,
                Some("POST") => 2,
                _ => return Err("Unsupported HTTP method".into()),
            };
            if bits & bit != 0 {
                return Err("Duplicate HTTP method".into());
            }
            bits |= bit;
        }
        if bits == 0 {
            return Err("Empty HTTP methods".into());
        }
    }
    seen.clear();
    for broker in mqtt {
        object(broker, &["host", "port", "subscribe", "publish"])?;
        let host = broker["host"].as_str().ok_or("Expected MQTT host")?;
        if !domain(host) || !seen.insert(host) || broker["port"].as_u64() != Some(8883) {
            return Err("Expected unique explicit MQTT TLS broker on port 8883".into());
        }
        for key in ["subscribe", "publish"] {
            if broker.get(key).is_none()
                || array(broker.get(key), 8)?
                    .iter()
                    .any(|p| !p.as_str().is_some_and(topic))
            {
                return Err("Invalid MQTT topic filters".into());
            }
        }
    }
    if let Some(c) = o.get("credentials") {
        object(c, &["records", "totalBytes"])?;
        for (k, max) in [("records", 8), ("totalBytes", 8192)] {
            if !c[k].as_u64().is_some_and(|n| n >= 1 && n <= max) {
                return Err("Invalid credential quota".into());
            }
        }
    }
    if https.is_empty() && mqtt.is_empty() && !o.contains_key("credentials") {
        return Err("Empty network policy".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #[test]
    fn network_install_requires_the_current_peer_capabilities() {
        use crate::clawpkg::ComponentSyncRequirements;
        use serde_json::json;
        let mut widget = json!({"hachiWasm":"digest","hachiNetwork":{
            "profile":super::PROFILE,"https":[{"origin":"https://api.example.org","methods":["GET"]}]
        }});
        let mut caps = json!({"widgetWasm":"hachi-wasm-v1"});
        let public = ComponentSyncRequirements::from_runtime(&widget);
        assert!(public.check_usb(&caps).is_err());
        assert!(public.check_unnegotiated_transport().is_err());
        caps["widgetWasmNetwork"] = json!(super::PROFILE);
        public.check_usb(&caps).unwrap();
        widget["hachiNetwork"]["credentials"] = json!({"records":1,"totalBytes":128});
        let private = ComponentSyncRequirements::from_runtime(&widget);
        for unsupported in [json!(null), json!(true), json!(super::PROFILE)] {
            caps["widgetComponentCredentials"] = unsupported;
            assert!(private.check_usb(&caps).is_err());
        }
        caps["widgetComponentCredentials"] = json!(super::CREDENTIAL_PROFILE);
        private.check_usb(&caps).unwrap();
        assert!(private.check_unnegotiated_transport().is_err());
        assert!(crate::clawpkg::validate_network_declaration(&widget, None).is_err());
        ComponentSyncRequirements::from_runtime(&json!({})).check_usb(&json!({})).unwrap();
    }

    #[test]
    fn policy_contract() {
        let cases: serde_json::Value = serde_json::from_str(include_str!(
            "../../../firmware/tests/component_network_policies.json"
        ))
        .unwrap();
        for case in cases.as_array().unwrap() {
            assert_eq!(
                super::validate(&case["policy"]).is_ok(),
                case["valid"].as_bool().unwrap(),
                "{case}"
            );
        }
    }
}
