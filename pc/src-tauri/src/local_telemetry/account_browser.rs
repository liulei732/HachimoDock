//! Explicit Chrome handoff for native-owned OpenAI login URLs. No shell.
#[cfg(target_os = "windows")]
use std::path::PathBuf;
use std::{
    process::{Command, Stdio},
    time::{Duration, Instant},
};

pub(super) fn valid_url(raw: &str) -> bool {
    reqwest::Url::parse(raw).is_ok_and(|url| {
        raw.len() <= 8192
            && url.scheme() == "https"
            && url.host_str() == Some("auth.openai.com")
            && url.port_or_known_default() == Some(443)
            && url.username().is_empty()
            && url.password().is_none()
    })
}
pub(super) fn open_chrome(url: &str) -> Result<(), String> {
    if !valid_url(url) {
        return Err("登录页面无效".into());
    }
    #[cfg(target_os = "macos")]
    {
        let mut child = Command::new("/usr/bin/open")
            .args(["-b", "com.google.Chrome", url])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|_| "未能打开 Chrome，请确认已安装后重试")?;
        let start = Instant::now();
        loop {
            match child.try_wait() {
                Ok(Some(status)) => {
                    return if status.success() {
                        Ok(())
                    } else {
                        Err("未能打开 Chrome，请确认已安装后重试".into())
                    }
                }
                Ok(None) if start.elapsed() < Duration::from_secs(5) => {
                    std::thread::sleep(Duration::from_millis(25))
                }
                _ => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err("Chrome 未响应，请再次打开登录页".into());
                }
            }
        }
    }
    #[cfg(target_os = "windows")]
    {
        for variable in ["LOCALAPPDATA", "PROGRAMFILES", "PROGRAMFILES(X86)"] {
            if let Some(base) = std::env::var_os(variable) {
                let binary = PathBuf::from(base).join("Google/Chrome/Application/chrome.exe");
                if binary.is_file() {
                    Command::new(binary)
                        .arg(url)
                        .stdin(Stdio::null())
                        .stdout(Stdio::null())
                        .stderr(Stdio::null())
                        .spawn()
                        .map_err(|_| "未能打开 Chrome，请确认已安装后重试")?;
                    return Ok(());
                }
            }
        }
        Err("未找到 Chrome，请安装后再次打开登录页".into())
    }
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    {
        Err("当前平台尚未接入 Chrome 登录启动器".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn accepts_only_owned_https_openai_urls_without_userinfo_or_other_ports() {
        assert!(valid_url(
            "https://auth.openai.com/oauth/authorize?state=test&code_challenge=test"
        ));
        for bad in [
            "http://auth.openai.com/oauth/authorize",
            "https://auth.openai.com.evil.test/",
            "https://example.com/",
            "https://user:password@auth.openai.com/",
            "https://auth.openai.com:1443/",
            "file:///tmp/anything",
        ] {
            assert!(!valid_url(bad));
        }
        assert!(!valid_url(&format!(
            "https://auth.openai.com/?q={}",
            "x".repeat(8192)
        )));
    }
}
