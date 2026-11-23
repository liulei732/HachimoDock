#[path = "../src/claude_hook_readiness.rs"]
mod readiness;

use serde_json::{json, Value};
use std::{fs, path::PathBuf, process::Command};

#[test]
fn discovery_accepts_installer_output_and_rejects_partial_or_unusable_hooks_read_only() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("settings.json");
    assert!(!readiness::registered(&path));
    for raw in ["{", "{}", r#"{"hooks":{"Stop":[{"command":"notify"}]}}"#] {
        fs::write(&path, raw).unwrap();
        assert!(!readiness::registered(&path));
        assert_eq!(fs::read_to_string(&path).unwrap(), raw);
    }
    let installer = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("bridge/hooks/install.js");
    let fixture = r#"require(process.argv[1]).registerHooks({settingsPath:process.argv[2],
        nodeBin:'node',port:23333,silent:true,claudeVersionInfo:{status:'unknown'}})"#;
    assert!(Command::new("node")
        .args(["-e", fixture])
        .arg(installer)
        .arg(&path)
        .status()
        .unwrap()
        .success());
    let installed = fs::read(&path).unwrap();
    assert!(readiness::registered(&path));
    assert_eq!(fs::read(&path).unwrap(), installed);

    let base: Value = serde_json::from_slice(&installed).unwrap();
    let group = base["hooks"]["Stop"].as_array().unwrap().last().unwrap();
    let command = group["hooks"][0]["command"].as_str().unwrap();
    for invalid in [
        json!([]),
        json!([{"type":"http","command":command}]),
        json!([{"type":"command","command":command.replace(" Stop"," SessionStart")}]),
        json!([{"command":command.replace("clawd-hook.js","not-clawd-hook.js")}]),
        json!([{"command":command.replace("hooks/clawd-hook.js","missing/hooks/clawd-hook.js")}]),
        json!([{"matcher":"only-some-tools","hooks":[{"type":"command","command":command}]}]),
    ] {
        let mut settings = base.clone();
        settings["hooks"]["Stop"] = invalid;
        fs::write(&path, settings.to_string()).unwrap();
        assert!(!readiness::registered(&path));
    }
    let mut settings = base;
    let nested = settings["hooks"]["UserPromptSubmit"][0]["hooks"][0].clone();
    settings["hooks"]["UserPromptSubmit"] = json!([nested]); // legacy direct command
    fs::write(&path, settings.to_string()).unwrap();
    assert!(readiness::registered(&path));
    settings["disableAllHooks"] = json!(true);
    fs::write(&path, settings.to_string()).unwrap();
    assert!(!readiness::registered(&path));
}
