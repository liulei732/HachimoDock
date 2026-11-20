//! Desktop IPC adapter; the account service itself is tested without Tauri.
#[tauri::command]
pub async fn codex_accounts_request(input: serde_json::Value) -> Result<serde_json::Value, String> {
    #[cfg(windows)]
    {
        let _ = input;
        return Err("Windows 账号管理尚未启用；需要系统凭据隔离支持".into());
    }
    tauri::async_runtime::spawn_blocking(move || {
        crate::local_telemetry::accounts::manager::request(input)
    })
    .await
    .map_err(|_| "账号管理任务未完成，请刷新检查".to_string())?
}
