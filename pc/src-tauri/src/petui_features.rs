//! Display metadata derived from validated package declarations. No authority to install or run.
use serde_json::{json, Value};
use std::{collections::HashSet, sync::OnceLock};

fn contract() -> &'static Value {
    static CONTRACT: OnceLock<Value> = OnceLock::new();
    CONTRACT.get_or_init(|| {
        serde_json::from_str(include_str!(
            "../../skills/petui/contracts/feature-compatibility-v1.json"
        ))
        .unwrap()
    })
}

pub(crate) fn manifest(id: &str, runtime: &Value, app: Option<&Value>, extension: bool) -> Value {
    let app = app.unwrap_or(&Value::Null);
    let fetch_rows = app["engine"] == crate::pc_fetch::ENGINE
        && runtime["data"]["source"]
            .as_str()
            .and_then(|s| s.strip_prefix("fetch."))
            .is_some_and(|id| {
                app["content"].as_array().is_some_and(|items| {
                    items
                        .iter()
                        .any(|v| v["id"] == id && v["map"]["rows"].is_object())
                })
            });
    let python = app
        .pointer("/content/0/sources")
        .and_then(Value::as_object)
        .and_then(|sources| sources.values().find(|v| v["kind"] == "python"))
        .and_then(|v| v.pointer("/input/kind"));
    let alarm = runtime["alarms"]
        .as_array()
        .is_some_and(|items| items.iter().any(|v| v["background"] == true));
    // Borrow the large runtime/application documents. Only fixed public rule fields leave this function.
    let derived = json!({"id":id,"extension":extension,"fetchRows":fetch_rows,"pythonInput":python,"backgroundAlarm":alarm});
    let lookup = |path: &str| {
        if let Some(rest) = path.strip_prefix("/runtime") {
            return runtime.pointer(rest);
        }
        if let Some(rest) = path.strip_prefix("/app") {
            return app.pointer(rest);
        }
        derived.pointer(path)
    };
    let mut groups = HashSet::new();
    let mut features = Vec::new();
    let mut feed_requirements = Value::Null;
    for rule in contract()["rules"].as_array().unwrap() {
        let group = rule["group"].as_str().unwrap();
        if groups.contains(group) || (group == "fallback" && !features.is_empty()) {
            continue;
        }
        if !rule["when"].as_object().unwrap().iter().all(|(key, want)| {
            if want == "*" {
                lookup(key).is_some_and(|v| !v.is_null())
            } else {
                lookup(key).unwrap_or(&Value::Null) == want
            }
        }) {
            continue;
        }
        groups.insert(group);
        let mut row = json!({"id":rule["id"], "label":rule["label"], "paths":rule["paths"]});
        if let Some(boards) = rule.get("deviceBoards") {
            row["deviceBoards"] = boards.clone();
        }
        if let Some(pc) = rule.get("pcRequires") {
            row["pcRequires"] = pc.clone();
        }
        if rule["optional"] == true {
            row["optional"] = true.into();
        }
        if let Some(grant) = rule.get("grant") {
            row["grant"] = grant.clone();
        }
        let mut required = rule["requires"].as_object().cloned().unwrap_or_default();
        for item in contract()["capabilities"].as_array().unwrap() {
            if item["group"] == rule["capsGroup"]
                && lookup(item["when"].as_str().unwrap()).is_some()
            {
                required.extend(item["requires"].as_object().unwrap().clone());
            }
        }
        if !required.is_empty() {
            row["requires"] = required.into();
        }
        if rule["capsGroup"] == "feed" {
            feed_requirements = row["requires"].clone();
        }
        if rule["id"] == "feed-refresh" {
            row["requires"] = feed_requirements.clone();
        }
        features.push(row);
    }
    json!({"schemaVersion":1,"runtime":runtime["engine"],"features":features})
}

pub(super) fn evaluate(profile: &Value, diagnostics: Option<&Value>) -> Value {
    let diag = diagnostics.filter(|d| d["ok"] != false && d["connected"] != false);
    let caps = diag.map(|d| {
        d.get("capabilities")
            .or_else(|| d.get("usbCapabilities"))
            .unwrap_or(d)
    });
    let board = caps.and_then(|c| ["hardwareBoardModel", "boardModel"].iter()
        .filter_map(|key| c["firmwareUpdate"][key].as_str())
        .find(|s| matches!(*s, "wifi6" | "pico")));
    let pico = board == Some("pico");
    let wifi = diag.and_then(|d| d["wifiConnected"].as_bool());
    let runtime = caps.and_then(|c| {
        let primary = c["widgetRuntime"].as_str();
        let versions = c["widgetRuntimes"].as_array();
        (primary.is_some() || versions.is_some()).then(|| {
            c["widgetRuntime"] == profile["runtime"]
                || versions.is_some_and(|v| v.contains(&profile["runtime"]))
        })
    });
    let mut rows = Vec::new();
    for row in profile["features"].as_array().into_iter().flatten() {
        let mut paths = serde_json::Map::new();
        for path in row["paths"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
        {
            let device = matches!(path, "device" | "device-wifi");
            let wireless = matches!(path, "device-wifi" | "paired-pc-wifi");
            let mut missing = false;
            let mut mismatch = device && board.is_some_and(|model| row["deviceBoards"]
                .as_array().is_some_and(|boards| !boards.iter().any(|b| b == model)));
            let requirements = if device {
                &row["requires"]
            } else {
                &row["pcRequires"]
            };
            for (key, want) in requirements.as_object().into_iter().flatten() {
                let actual = caps.and_then(|c| c.get(key));
                let alternative = key == "widgetProgram"
                    && caps.is_some_and(|c| {
                        c["widgetPrograms"]
                            .as_array()
                            .is_some_and(|v| v.contains(want))
                    });
                if !alternative {
                    missing |= actual.is_none();
                    mismatch |= actual.is_some_and(|v| v != want);
                }
            }
            let status = if path == "unknown" {
                "unknown"
            } else if wireless && pico
                || mismatch
                || (device || requirements.is_object()) && runtime == Some(false)
            {
                "unsupported"
            } else if wireless && wifi == Some(false) {
                "offline"
            } else if missing || (device || requirements.is_object()) && runtime.is_none() {
                "unconfirmed"
            } else if !device {
                "pc-required"
            } else if wireless && wifi.is_none() {
                "network-unconfirmed"
            } else {
                "capabilities-match"
            };
            paths.insert(path.into(), status.into());
        }
        rows.push(json!({"id":row["id"],"paths":paths}));
    }
    Value::Array(rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn foreground_meter_examples_use_negotiated_delivery() {
        for name in ["mic-level-meter", "quiet-room-check"] {
            let folder = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills/petui/examples").join(name);
            let entry = crate::component_library::inspect(&folder).unwrap();
            assert!(entry.valid, "{:?}", entry.validation_errors);
            let caps = json!({"widgetMicLevel":"microphone-level-v1"});
            assert!(entry.sync_requirements.check_usb(&caps).is_ok());
            assert!(entry.sync_requirements.check_usb(&json!({"audio":true})).is_err());
            assert!(entry.sync_requirements.check_unnegotiated_transport().is_err());
            let mut widget: Value = serde_json::from_slice(&std::fs::read(folder.join("runtime/widget.json")).unwrap()).unwrap();
            crate::clawpkg::validate_p4_runtime_shell(&widget).unwrap();
            for field in ["raw", "grant", "background"] {
                widget["hardware"][field] = true.into();
                assert!(crate::clawpkg::validate_p4_runtime_shell(&widget).is_err());
                widget["hardware"].as_object_mut().unwrap().remove(field);
            }
            for n in [json!(0), json!(101), json!("20"), json!(true)] {
                widget["hardware"]["quiet_below"] = n;
                assert!(crate::clawpkg::validate_p4_runtime_shell(&widget).is_err());
            }
        }
    }

    #[test]
    fn cross_surface_manifest_and_target_vectors() {
        let fixtures: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/feature-compatibility-cases.json"
        ))
        .unwrap();
        for case in fixtures.as_array().unwrap() {
            let manifest = manifest("test", &case["runtime"], case.get("app"), false);
            let ids: Vec<_> = manifest["features"]
                .as_array()
                .unwrap()
                .iter()
                .map(|r| &r["id"])
                .collect();
            assert_eq!(json!(ids), case["ids"], "{}", case["case"]);
            for target in case["targets"].as_array().unwrap() {
                let rows = evaluate(&manifest, target.get("diagnostic"));
                let row = rows
                    .as_array()
                    .unwrap()
                    .iter()
                    .find(|r| r["id"] == target["feature"])
                    .unwrap();
                assert_eq!(row["paths"], target["paths"], "{}", case["case"]);
            }
        }
    }
}

#[cfg(test)]
#[test]
fn live_authoring_distribution_refreshes_existing_skill_in_isolated_home() {
    let src = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills/petui");
    let target = tempfile::tempdir().unwrap();
    let installed = target.path().join(".agents/skills/petui");
    std::fs::create_dir_all(&installed).unwrap();
    std::fs::write(installed.join("SKILL.md"), "previous revision").unwrap();
    crate::sync_widget_skills(&src, target.path()).unwrap();
    for file in [
        "SKILL.md",
        "agents/openai.yaml",
        "references/authoring-inspection.md",
        "contracts/feature-compatibility-v1.json",
        "scripts/feature_profile.py",
        "references/foreground-hardware.md",
        "examples/mic-level-meter/runtime/widget.json",
        "examples/quiet-room-check/runtime/widget.json",
    ] {
        assert_eq!(
            std::fs::read(src.join(file)).unwrap(),
            std::fs::read(installed.join(file)).unwrap()
        );
    }
    let info: Value = serde_json::from_str(&super::cli(&["tool-info".into()]).unwrap()).unwrap();
    assert_eq!(info["featureManifest"], 1);
}
