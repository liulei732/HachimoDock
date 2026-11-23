use regex::Regex;
use serde_json::Value;
use std::{fs, path::Path, sync::OnceLock};

// The lifecycle observer installed by bridge/hooks/install.js. Optional,
// version-dependent hooks and permission decisions have separate contracts.
const CORE_EVENTS: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "Stop",
    "SubagentStart",
    "SubagentStop",
    "Notification",
    "Elicitation",
];

fn observer(hook: &Value, event: &str) -> bool {
    if hook.get("type").is_some_and(|kind| kind != "command") {
        return false;
    }
    static COMMAND: OnceLock<Regex> = OnceLock::new();
    let pattern = COMMAND.get_or_init(|| {
        Regex::new(
            r#"^(?:CLAWD_REMOTE=1\s+)?(?:"([^"]+)"|([^\s"]+))\s+(?:"([^"]+)"|([^\s"]+))\s+(\w+)$"#,
        )
        .expect("observer command pattern")
    });
    let Some(args) = hook["command"]
        .as_str()
        .and_then(|s| pattern.captures(s.trim()))
    else {
        return false;
    };
    let runtime = Path::new(args.get(1).or(args.get(2)).unwrap().as_str());
    let script = Path::new(args.get(3).or(args.get(4)).unwrap().as_str());
    runtime
        .file_name()
        .is_some_and(|name| name == "node" || name == "node.exe")
        && (runtime.components().count() == 1 || runtime.is_file())
        && script.ends_with("hooks/clawd-hook.js")
        && script.is_absolute()
        && script.is_file()
        && &args[5] == event
}

pub(super) fn registered(settings_path: &Path) -> bool {
    if !settings_path.is_file() {
        return false;
    }
    let Some(settings) = fs::read(settings_path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
    else {
        return false;
    };
    settings["disableAllHooks"] != true
        && CORE_EVENTS.iter().all(|event| {
            settings["hooks"][event].as_array().is_some_and(|groups| {
                groups.iter().any(|group| {
                    group
                        .get("matcher")
                        .is_none_or(|value| matches!(value.as_str(), Some("" | "*")))
                        && (observer(group, event)
                            || group["hooks"].as_array().is_some_and(|hooks| {
                                hooks.iter().any(|hook| observer(hook, event))
                            }))
                })
            })
        })
}
