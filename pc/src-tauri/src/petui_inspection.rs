//! Read installed package metadata in a disposable library using the production importer.
use super::{data_link, local_resources};
use crate::{app_services, clawpkg, component_composition, component_library, component_python};
use component_library::ComponentLibraryEntry as Entry;
use serde_json::{json, Value};
use std::{ffi::OsString, path::Path};

pub(super) const USAGE: &str = "petui compatibility <包> [--diagnostics JSON] | first-data <包> [--fixture JSON] [--diagnostics JSON] | install-rehearsal <包> [--fixture JSON] [--upgrade 新包]";

fn read_json(path: &Path, limit: u64) -> Result<Value, String> {
    let bytes = clawpkg::read_clawpkg_source_file(path, limit)?;
    serde_json::from_slice(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&bytes))
        .map_err(|_| "无效的 JSON 输入".into())
}

fn compatibility(entry: &Entry, runtime: &Value, diagnostics: Option<&Value>) -> Value {
    let data = &runtime["data"];
    let source = data["source"].as_str();
    let app = entry.pc_application.as_ref();
    let private_account = matches!(
        data["connection"]["type"].as_str(),
        Some("bearer" | "mqtt" | "https-submit" | "mqtt-publish")
    );
    let profile = super::features::manifest(&entry.id, runtime, app, entry.pc_extension.is_some());
    let feature_status = super::features::evaluate(&profile, diagnostics);
    let features = profile["features"].as_array().unwrap();
    // Preserve the CLI's protocol names; the feature contract owns all path choices.
    let routes: Vec<Value> = features
        .iter()
        .map(|f| {
            let id = f["id"].as_str().unwrap();
            let feature = match id {
                "feed-refresh"
                | "refresh-fetch"
                | "refresh-computer.status"
                | "refresh-todos.upcoming"
                | "refresh-unknown" => "data.refresh",
                "provider" | "fetch" | "stocks" => source.unwrap_or(id),
                "headlines" => "news.hn",
                "stats" => "stats/update",
                "extension" => "pc-extension",
                "host" | "app" | "fetch-host" => "app/snapshot",
                "wifi-audio" | "radio" | "media" => entry.media_source.as_deref().unwrap_or(id),
                "local" | "game" | "local-tool" => "local-runtime",
                name if name.starts_with("feed-") => "wifi.feed",
                name if name.starts_with("python-") => "app/snapshot",
                _ => id,
            };
            let paths = if id == "provider" {
                if source == Some("codex.usage") {
                    json!(["provider-unavailable"])
                } else {
                    json!(["check-provider"])
                }
            } else {
                f["paths"].clone()
            };
            json!({"feature":feature,"paths":paths})
        })
        .collect();
    let mut required = json!(entry.sync_requirements)["usbCapabilities"].clone();
    if entry.pc_application.is_some() {
        required["widgetAppHost"] = "p4-app-host-v1".into();
    }
    let caps = diagnostics
        .filter(|d| d.get("ok").is_none_or(|ok| ok == true))
        .and_then(|d| {
            d.get("capabilities")
                .or_else(|| d.get("usbCapabilities"))
                .or(Some(d))
        });
    let caps = caps.filter(|value| value.is_object());
    let check = |fields: &Value| {
        let mut checks = serde_json::Map::new();
        for (name, expected) in fields.as_object().into_iter().flatten() {
            let status = match caps.and_then(|v| v.get(name)) {
                None => "unreported",
                Some(v) if v == expected => "match",
                _ => "mismatch",
            };
            checks.insert(name.clone(), json!({"required":expected,"status":status}));
        }
        checks
    };
    let checks = check(&required);
    // A read-time requirement must not block installing a package with a PC fallback.
    let mut direct = json!({});
    for row in features {
        if row["paths"]
            .as_array()
            .is_some_and(|p| p.iter().any(|v| v == "device-wifi"))
        {
            for (key, value) in row["requires"].as_object().into_iter().flatten() {
                direct[key] = value.clone();
            }
        }
    }
    let direct_checks = check(&direct);
    let plan = app
        .and_then(|a| a.pointer("/content/0/sources"))
        .and_then(Value::as_object);
    let mut sources = Vec::new();
    for (name, spec) in plan.into_iter().flatten() {
        sources.push(json!({"name":name,"kind":spec["kind"],"input":spec["input"]["kind"]}));
    }
    let permissions = app
        .and_then(|v| serde_json::from_value::<app_services::Declaration>(v.clone()).ok())
        .map(|decl| app_services::permissions(&decl));
    let mut grants = Vec::new();
    if matches!(
        data["connection"]["type"].as_str(),
        Some("https-submit" | "mqtt-publish")
    ) {
        grants.push("device-one-shot-send");
    }
    if app.is_some_and(|declaration| declaration["engine"] == crate::pc_fetch::ENGINE) {
        grants.extend(["enable-pc-fetch", "allow-declared-host"]);
    }
    if source == Some("github.me") {
        grants.push("pc-service-account");
    }
    for spec in plan.into_iter().flatten().map(|(_, v)| v) {
        if spec["kind"] == "python" {
            grants.push("enable-python");
            match spec["input"]["kind"].as_str() {
                Some("text-file") => grants.push("select-local-file"),
                Some("https-json") => grants.push("allow-declared-host"),
                _ => (),
            }
        }
    }
    let standalone = routes.iter().all(|r| {
        r["paths"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == "device" || v == "device-wifi")
    });
    let mut device_states = Vec::new();
    let mut matching_features = 0;
    for status in feature_status.as_array().unwrap() {
        let paths = &status["paths"];
        let mut matched = false;
        for key in ["device", "device-wifi"] {
            if let Some(state) = paths[key].as_str() {
                device_states.push(state);
                matched |= matches!(state, "capabilities-match" | "network-unconfirmed");
            }
        }
        matching_features += usize::from(matched);
    }
    let target_autonomy = if device_states.is_empty() {
        "no-device-route"
    } else if entry.sync_requirements.blocked_reason.is_some()
        || device_states.iter().all(|s| *s == "unsupported")
    {
        "incompatible"
    } else if matching_features == features.len() {
        "declared-checks-match"
    } else if matching_features > 0 {
        "partial-device-route"
    } else if device_states.contains(&"unconfirmed") {
        "unconfirmed"
    } else {
        "network-offline"
    };
    json!({"featureManifest":profile,"featureStatus":feature_status,"component":entry.id,"canRunAllListedFeaturesWithoutPc":standalone,"targetAutonomy":target_autonomy,"routes":routes,
        "deviceDirectChecks":direct_checks,
        "feedProtocol":source.filter(|s| *s == "wifi.feed").map(|_| data_link::feed_protocol(runtime)),
        "hostSources":sources,"firstUse":grants,"pcPermissions":permissions,"deviceServiceAccount":private_account,
        "actions":app.and_then(|v|v["actions"].as_object()).map(|a|a.keys().collect::<Vec<_>>()),
        "deviceCapabilityChecks":checks,"installBlocked":entry.sync_requirements.blocked_reason})
}

fn first_data(
    entry: &Entry,
    home: &Path,
    runtime: &Value,
    fixture: Option<Value>,
    diag: Option<(&Path, &Value)>,
    inspect_installed: bool,
) -> Result<Value, String> {
    let app = entry.pc_application.as_ref();
    let plan = app.and_then(|v| v.pointer("/content/0"));
    let decl = app.cloned().and_then(|v| serde_json::from_value(v).ok());
    let mut outputs = fixture.clone();
    let mut plugin = Value::Null;
    let mut alias_filter = None;
    if let Some(spec) = decl.as_ref().and_then(component_composition::python_spec) {
        let alias = plan
            .and_then(|p| p["sources"].as_object())
            .and_then(|m| m.iter().find(|(_, v)| v["kind"] == "python"))
            .ok_or("缺少 Python 来源")?
            .0;
        let (result, value) =
            component_python::rehearsal(home, &local_resources()?, &entry.id, &spec, fixture)?;
        plugin = result;
        if inspect_installed {
            plugin["installedInput"] =
                component_python::local_input_status(&entry.id, &entry.version_hash, &spec).into();
        }
        outputs = value.map(|v| json!({(alias):v}));
        alias_filter = Some(alias);
    }
    // Local providers can open through the normal composition host. External providers
    // require separate observations or an explicitly supplied fixture.
    let mut host_snapshot = Value::Null;
    if outputs.is_none() {
        if let Some(d) = decl
            .as_ref()
            .filter(|d| component_composition::is_engine(&d.engine))
        {
            let local = plan
                .and_then(|v| v["sources"].as_object())
                .is_some_and(|sources| {
                    sources.values().all(|v| {
                        matches!(
                            v["kind"].as_str(),
                            Some(
                                "constant"
                                    | "clock"
                                    | "counter"
                                    | "timer"
                                    | "ledger"
                                    | "computed"
                                    | "document"
                                    | "collection"
                            )
                        )
                    })
                });
            if local {
                let state_dir = home.join("composition-state");
                std::fs::create_dir_all(&state_dir).map_err(|_| "无法打开预演状态目录")?;
                let snapshot = component_composition::request(
                    &entry.id,
                    &state_dir,
                    d,
                    runtime,
                    &json!({"operation":"status"}),
                )?;
                host_snapshot = json!({"sources":snapshot["sources"],"bindingErrors":snapshot["bindingErrors"],"variableCount":snapshot["variables"].as_object().map(|v|v.len())});
            }
        }
    }
    let mut bindings = Vec::new();
    for (key, b) in plan
        .and_then(|v| v["bindings"].as_object())
        .into_iter()
        .flatten()
    {
        let alias = b["source"].as_str().ok_or("无效绑定来源")?;
        let ptr = b["pointer"].as_str().ok_or("无效绑定字段")?;
        let status = match outputs.as_ref() {
            None if host_snapshot.is_object() => {
                if host_snapshot["bindingErrors"]
                    .as_array()
                    .is_some_and(|v| v.iter().any(|b| b == key))
                {
                    "fallback"
                } else {
                    "projected"
                }
            }
            None => "unobserved",
            Some(_) if alias_filter.is_some_and(|a| a != alias) => "unobserved",
            Some(data) => match data.get(alias).and_then(|v| v.pointer(ptr)) {
                None => "missing-field",
                Some(v) if component_composition::project_binding(runtime, key, v).is_some() => {
                    "projected"
                }
                _ => "type-or-limit",
            },
        };
        bindings.push(json!({"variable":key,"source":alias,"pointer":ptr,"status":status}));
    }
    let mut active = "unobserved";
    let mut receipt = Value::Null;
    if let Some((path, d)) = diag.filter(|(_, d)| d.get("ok").is_none_or(|ok| ok == true)) {
        if let Some(id) = d.get("activeWidgetId") {
            active = if id.as_str() == Some(&entry.id) {
                "match"
            } else {
                "different"
            };
        }
        if runtime.get("data").is_some() {
            let report = data_link::run(
                &Path::new(&entry.path).join("runtime/widget.json"),
                path,
                &[],
            )?;
            receipt = serde_json::from_str(&report).map_err(|_| "接收证据无效")?;
        }
    }
    Ok(
        json!({"component":entry.id,"previewGrant":plugin,"hostSnapshot":host_snapshot,"bindings":bindings,"activeComponent":active,"sourceReceipt":receipt}),
    )
}

pub(super) fn run(command: &str, args: &[OsString]) -> Result<String, String> {
    let (package, mut rest) = args.split_first().ok_or(USAGE)?;
    let (mut fixture, mut diag, mut upgrade) = (None, None, None);
    while let [flag, path, tail @ ..] = rest {
        match flag.to_str() {
            Some("--fixture") if command != "compatibility" && fixture.is_none() => {
                fixture = Some(read_json(Path::new(path), 24 * 1024)?)
            }
            Some("--diagnostics") if command != "install-rehearsal" && diag.is_none() => {
                diag = Some((Path::new(path), read_json(Path::new(path), 64 * 1024)?))
            }
            Some("--upgrade") if command == "install-rehearsal" && upgrade.is_none() => {
                upgrade = Some(path)
            }
            _ => return Err(USAGE.into()),
        }
        rest = tail;
    }
    if !rest.is_empty() {
        return Err(USAGE.into());
    }
    let clean = tempfile::tempdir().map_err(|e| e.to_string())?;
    let open = |path: &OsString| -> Result<(Entry, Value), String> {
        let e = component_library::import_hachiapp(clean.path(), Path::new(path))?;
        let current = component_library::latest(clean.path(), &e.id)?.ok_or("导入后组件不可见")?;
        if !current.valid || current.version_hash != e.version_hash {
            return Err("新导入的版本未成为当前可用版本".into());
        }
        let r = read_json(
            &Path::new(&e.path).join("runtime/widget.json"),
            clawpkg::CLAWPKG_MAX_ENTRY_BYTES,
        )?;
        Ok((e, r))
    };
    let (entry, runtime) = open(package)?;
    let report = match command {
        "compatibility" => compatibility(&entry, &runtime, diag.as_ref().map(|(_, d)| d)),
        "first-data" => first_data(
            &entry,
            clean.path(),
            &runtime,
            fixture,
            diag.as_ref().map(|(p, d)| (*p, d)),
            true,
        )?,
        "install-rehearsal" => {
            let mut result = json!({"component":entry.id,"version":entry.version,"firstOpen":"library-entry-verified",
                "compatibility":compatibility(&entry,&runtime,None),"firstData":first_data(&entry,clean.path(),&runtime,fixture.clone(),None,false)?});
            if let Some(next) = upgrade {
                let (updated, r) = open(next)?;
                if updated.id != entry.id || updated.version_hash == entry.version_hash {
                    return Err("升级预演需要同一组件 ID 的不同内容版本".into());
                }
                result["upgrade"] = json!({"version":updated.version,"firstData":first_data(&updated,clean.path(),&r,fixture,None,false)?});
            }
            result
        }
        _ => return Err(USAGE.into()),
    };
    serde_json::to_string_pretty(&report).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn package(path: &str) -> (Entry, Value) {
        let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(path);
        let entry = component_library::inspect(&path).unwrap();
        assert!(entry.valid, "{:?}", entry.validation_errors);
        let runtime = read_json(
            &path.join("runtime/widget.json"),
            clawpkg::CLAWPKG_MAX_ENTRY_BYTES,
        )
        .unwrap();
        (entry, runtime)
    }

    #[test]
    fn compatibility_separates_optional_stock_feed_from_install_and_receipts() {
        let (entry, runtime) = package("builtin-clawpkgs/stock-watchlist");
        for board in ["wifi6", "pico"] {
            for revision in [103, 302] {
                let mut caps = json!({"widgetRuntime":"p4-bounded-runtime-v4","widgetState":"p4-app-state-v1","firmwareUpdate":{"hardwareBoardModel":board,"chipRevision":revision}});
                if board == "wifi6" {
                    caps["widgetWifiData"] = "p4-wifi-data-v1".into();
                }
                let r = compatibility(&entry, &runtime, Some(&json!({"capabilities":caps})));
                assert_eq!(
                    r["routes"][0]["paths"],
                    json!(["device-wifi", "pc-usb", "paired-pc-wifi"])
                );
                assert!(r["deviceCapabilityChecks"].get("widgetWifiData").is_none());
                assert_eq!(
                    r["targetAutonomy"],
                    "partial-device-route"
                );
            }
        }
        let counts = json!({"widgetData":{"stocks.watchlist":{"received":true,"wifiCount":4}}});
        assert_eq!(
            compatibility(&entry, &runtime, Some(&counts))["targetAutonomy"],
            "unconfirmed"
        );
    }

    #[test]
    fn compatibility_reports_private_storage_separately_from_install_checks() {
        let (mut entry, mut runtime) = package("builtin-clawpkgs/stock-watchlist");
        runtime.as_object_mut().unwrap().remove("persist");
        for (kind, cap, version) in [
            ("bearer", "widgetPrivateFeed", "p4-private-feed-v1"),
            ("mqtt", "widgetPrivateFeed", "p4-private-feed-v1"),
            ("mqtt-public", "widgetPublicMqtt", "p4-public-mqtt-v1"),
        ] {
            runtime["data"] = json!({"source":"wifi.feed","connection":{"type":kind},"refresh_action":"data.refresh"});
            entry.sync_requirements = clawpkg::ComponentSyncRequirements::from_runtime(&runtime);
            let mut caps = json!({"widgetRuntime":"p4-bounded-runtime-v4","widgetWifiFeed":"p4-wifi-feed-v1","widgetCredentialStorageReady":false});
            caps[cap] = version.into();
            let r = compatibility(&entry, &runtime, Some(&caps));
            assert_eq!(
                r["feedProtocol"],
                if kind == "bearer" {
                    "https"
                } else {
                    "mqtt-tls"
                }
            );
            assert_eq!(r["deviceServiceAccount"], kind != "mqtt-public");
            assert_eq!(
                r["routes"][1],
                json!({"feature":"data.refresh","paths":["device-wifi"]})
            );
            assert!(r["deviceCapabilityChecks"]
                .get("widgetCredentialStorageReady")
                .is_none());
            assert_eq!(
                r["targetAutonomy"],
                if kind == "mqtt-public" {
                    "declared-checks-match"
                } else {
                    "incompatible"
                }
            );
            caps["widgetCredentialStorageReady"] = true.into();
            assert_eq!(
                compatibility(&entry, &runtime, Some(&caps))["targetAutonomy"],
                "declared-checks-match"
            );
        }
    }

    #[test]
    fn compatibility_only_lists_implemented_sources_and_declared_refresh_actions() {
        let (entry, mut runtime) = package("builtin-clawpkgs/computer-status");
        runtime.as_object_mut().unwrap().remove("persist");
        runtime["data"]["refresh_action"] = "data.refresh".into();
        let r = compatibility(&entry, &runtime, None);
        assert_eq!(
            r["routes"][1],
            json!({"feature":"data.refresh","paths":["pc-usb"]})
        );
        runtime["data"]
            .as_object_mut()
            .unwrap()
            .remove("refresh_action");
        for name in [
            "news.hn",
            "fetch.undeclared",
            "invented.name",
            "codex.usage",
        ] {
            runtime["data"]["source"] = name.into();
            let r = compatibility(&entry, &runtime, None);
            assert_eq!(r["routes"].as_array().unwrap().len(), 1);
            assert_eq!(
                r["routes"][0]["paths"],
                if name == "codex.usage" {
                    json!(["provider-unavailable"])
                } else {
                    json!(["check-provider"])
                }
            );
            assert_eq!(r["canRunAllListedFeaturesWithoutPc"], false);
        }
        let mut fetch_entry = entry;
        fetch_entry.pc_application = Some(json!({
            "engine": crate::pc_fetch::ENGINE, "content":[{"id":"weather","map":{"rows":{"items":"/items","id":"/id","label":"/label"}}}]
        }));
        runtime["data"]["source"] = "fetch.weather".into();
        let r = compatibility(&fetch_entry, &runtime, None);
        assert_eq!(r["routes"][0]["paths"], json!(["pc-usb", "paired-pc-wifi"]));
        assert!(r["routes"]
            .as_array()
            .unwrap()
            .iter()
            .all(|v| v["feature"] != "data.refresh"));
        assert_eq!(
            r["firstUse"],
            json!(["enable-pc-fetch", "allow-declared-host"])
        );
        runtime["data"]["refresh_action"] = "data.refresh".into();
        assert_eq!(
            compatibility(&fetch_entry, &runtime, None)["routes"][1]["paths"],
            json!(["pc-usb"])
        );

        let (entry, runtime) = package("optional-clawpkgs/daily-news");
        let r = compatibility(&entry, &runtime, None);
        assert_eq!(
            r["routes"][0],
            json!({"feature":"news.hn","paths":["device-wifi"]})
        );
        assert_eq!(r["routes"][1]["feature"], "app/snapshot");
        assert_eq!(r["canRunAllListedFeaturesWithoutPc"], false);
    }

    #[test]
    fn compatibility_checks_the_advertised_runtime_for_local_packages() {
        let (entry, runtime) = package("builtin-clawpkgs/inkline-gomoku");
        let engine = runtime["engine"].as_str().unwrap();
        let local =
            |caps: Value| compatibility(&entry, &runtime, Some(&caps))["targetAutonomy"].clone();
        assert_eq!(local(json!({"widgetRuntime":"other"})), "incompatible");
        assert_eq!(
            local(json!({"widgetRuntime":"other","widgetRuntimes":[engine]})),
            "declared-checks-match"
        );
        assert_eq!(local(json!({"widgetData":{}})), "unconfirmed");
    }

    #[test]
    fn compatibility_keeps_python_https_input_on_pc() {
        let (mut entry, _) = package("builtin-clawpkgs/computer-status");
        for (input, grant) in [
            ("none", None),
            ("text-file", Some("select-local-file")),
            ("https-json", Some("allow-declared-host")),
        ] {
            entry.pc_application = Some(
                json!({"engine":"pc-app-composition-v1","service":"composition",
                "content":[{"sources":{"metrics":{"kind":"python","entry":"pc/plugin.py","input":{"kind":input}}}}]}),
            );
            let r = compatibility(&entry, &json!({"host":true}), None);
            assert_eq!(
                r["routes"],
                json!([{"feature":"app/snapshot","paths":["pc-usb","paired-pc-wifi"]}])
            );
            assert_eq!(r["hostSources"][0]["input"], input);
            let mut grants = vec!["enable-python"];
            grants.extend(grant);
            assert_eq!(r["firstUse"], json!(grants));
            assert_eq!(r["deviceServiceAccount"], false);
            assert_eq!(r["targetAutonomy"], "no-device-route");
        }
    }
}
