//! Launch the bundled Win32 ACL verifier with a path and operation.
use serde_json::Value;

fn valid_path(path: &str) -> bool {
    let b = path.as_bytes();
    b.len() >= 4
        && b.len() <= 2048
        && b[0].is_ascii_alphabetic()
        && b[1] == b':'
        && b[2] == b'\\'
        && !path[2..].contains(':')
        && !path.contains('/')
        && !path.chars().any(|c| c < ' ')
        && path[3..].split('\\').all(|p| {
            !p.is_empty() && p != "." && p != ".." && !p.ends_with('.') && !p.ends_with(' ')
        })
}
fn validate_receipt(v: &Value, file: &str) -> Result<(), String> {
    let reject = || "unsafe Windows DSH capability ACL".to_string();
    let sid = v["currentSid"].as_str().ok_or_else(reject)?;
    if !valid_path(file)
        || v["version"] != 1
        || v["operation"] != "verify-file"
        || !sid.starts_with("S-1-")
        || sid.len() > 184
        || sid.split('-').count() < 4
        || !sid
            .split('-')
            .skip(2)
            .all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit()))
        || sid == "S-1-5-18"
    {
        return Err(reject());
    }
    let parent = file.rsplit_once('\\').ok_or_else(reject)?.0;
    for (key, path, directory) in [("directory", parent, true), ("item", file, false)] {
        let e = &v[key];
        let rules = e["rules"].as_array().ok_or_else(reject)?;
        if !e["path"]
            .as_str()
            .map(|p| p.eq_ignore_ascii_case(path))
            .unwrap_or(false)
            || e["directory"] != directory
            || e["owner"] != sid
            || e["protected"] != true
            || e["reparse"] != false
            || e["rawAceCount"] != 2
            || rules.len() != 2
        {
            return Err(reject());
        }
        let mut seen_user = false;
        let mut seen_system = false;
        for r in rules {
            if r["type"] != 0
                || r["inherited"] != false
                || r["rights"] != 2032127
                || r["inheritance"] != if directory { 3 } else { 0 }
                || r["propagation"] != 0
            {
                return Err(reject());
            }
            match r["sid"].as_str() {
                Some(s) if s == sid && !seen_user => seen_user = true,
                Some("S-1-5-18") if !seen_system => seen_system = true,
                _ => return Err(reject()),
            }
        }
        if !seen_user || !seen_system {
            return Err(reject());
        }
    }
    Ok(())
}

pub(crate) fn verify_file(file: &std::path::Path) -> Result<(), String> {
    use std::io::Read;
    #[cfg(windows)]
    use std::os::windows::process::CommandExt;
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};
    let target = file
        .to_str()
        .filter(|p| valid_path(p))
        .ok_or("invalid Windows DSH capability path")?;
    let root = std::env::var("SystemRoot")
        .or_else(|_| std::env::var("WINDIR"))
        .map_err(|_| "Windows SystemRoot unavailable")?;
    if !valid_path(&root) {
        return Err("invalid Windows SystemRoot".into());
    }
    let app = std::env::current_exe().map_err(|_| "Windows ACL helper path unavailable")?;
    let executable = app.parent().ok_or("Windows ACL helper path unavailable")?.join("dsh-bridge/windows-acl.exe");
    let metadata = std::fs::symlink_metadata(&executable).map_err(|_| "Windows native ACL helper unavailable")?;
    if !metadata.is_file() || metadata.file_type().is_symlink() {
        return Err("Windows native ACL helper unavailable".into());
    }
    let mut command = Command::new(executable);
    command
        .args(["capability", "verify-file", target])
        .env_clear()
        .env("SystemRoot", &root)
        .env("WINDIR", &root)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(windows)]
    command.creation_flags(0x08000000);
    let mut child = command
        .spawn()
        .map_err(|_| "Windows DSH ACL verifier unavailable")?;
    // Drain concurrently: do not wait for exit with a full anonymous pipe.
    let stdout = child
        .stdout
        .take()
        .ok_or("Windows ACL stdout unavailable")?;
    let stderr = child
        .stderr
        .take()
        .ok_or("Windows ACL stderr unavailable")?;
    let read = |stream: Box<dyn Read + Send>| {
        std::thread::spawn(move || {
            let mut bytes = Vec::new();
            stream.take(32769).read_to_end(&mut bytes).map(|_| bytes)
        })
    };
    let output = read(Box::new(stdout));
    let errors = read(Box::new(stderr));
    let deadline = Instant::now() + Duration::from_secs(5);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                break None;
            }
        }
    };
    let output = output
        .join()
        .map_err(|_| "Windows ACL output unavailable")?
        .map_err(|_| "Windows ACL output unavailable")?;
    let errors = errors
        .join()
        .map_err(|_| "Windows ACL errors unavailable")?
        .map_err(|_| "Windows ACL errors unavailable")?;
    if !status.map(|s| s.success()).unwrap_or(false) || output.len() > 32768 || !errors.is_empty() {
        return Err("Windows DSH ACL verification rejected".into());
    }
    let text = std::str::from_utf8(&output)
        .map_err(|_| "invalid Windows ACL receipt")?
        .trim_start_matches('\u{feff}')
        .trim();
    let receipt: Value = serde_json::from_str(text).map_err(|_| "invalid Windows ACL receipt")?;
    validate_receipt(&receipt, target)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn receipt() -> Value {
        let sid = "S-1-5-21-1-2-3-1001";
        let entry = |path: &str, directory: bool| {
            json!({"path":path,"directory":directory,"owner":sid,"protected":true,"reparse":false,"rawAceCount":2,"rules":[
            {"sid":sid,"type":0,"inherited":false,"rights":2032127,"inheritance":if directory {3} else {0},"propagation":0},
            {"sid":"S-1-5-18","type":0,"inherited":false,"rights":2032127,"inheritance":if directory {3} else {0},"propagation":0} ]})
        };
        json!({"version":1,"operation":"verify-file","currentSid":sid,"directory":entry(r"C:\private",true),"item":entry(r"C:\private\capability.json",false)})
    }
    #[test]
    fn windows_acl_requires_protected_current_user_and_system_only() {
        let v = receipt();
        assert!(validate_receipt(&v, r"C:\private\capability.json").is_ok());
        for key in ["directory", "item"] {
            for (field, value) in [
                ("owner", json!("S-1-5-32-544")),
                ("protected", json!(false)),
                ("reparse", json!(true)),
            ] {
                let mut bad = v.clone();
                bad[key][field] = value;
                assert!(validate_receipt(&bad, r"C:\private\capability.json").is_err());
            }
            let mut bad = v.clone();
            bad[key]["rules"][0]["inherited"] = json!(true);
            assert!(validate_receipt(&bad, r"C:\private\capability.json").is_err());
            let mut bad = v.clone();
            bad[key]["rules"][1]["sid"] = json!("S-1-1-0");
            assert!(validate_receipt(&bad, r"C:\private\capability.json").is_err());
        }
    }
    #[test]
    fn windows_capability_path_rejects_unc_devices_ads_and_ambiguous_segments() {
        for p in [
            r"\\server\share\capability.json",
            r"\\?\C:\private\capability.json",
            r"C:\private\..\capability.json",
            r"C:\private\capability.json:stream",
            r"C:\private \capability.json",
            "C:/private/capability.json",
        ] {
            assert!(!valid_path(p));
        }
        assert!(valid_path(r"C:\用户\private\capability.json"));
    }
}
