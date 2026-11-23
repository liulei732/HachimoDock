//! Desktop IPC adapter; the account service itself is tested without Tauri.
#[tauri::command]
pub async fn codex_accounts_request(input: serde_json::Value) -> Result<serde_json::Value, String> {
    #[cfg(not(target_os = "macos"))]
    {
        let _ = input;
        return Err("当前系统尚未启用 Codex 额度账号管理".into());
    }
    #[cfg(target_os = "macos")]
    tauri::async_runtime::spawn_blocking(move || {
        crate::local_telemetry::accounts::manager::request(input)
    })
    .await
    .map_err(|_| "账号管理任务未完成，请刷新检查".to_string())?
}

#[cfg(all(test, windows))]
mod tests {
    use super::codex_accounts_request;
    use crate::local_telemetry::accounts::manager::assert_no_account_writes;
    use serde_json::json;

    #[test]
    fn windows_rejects_account_commands_without_writing_credentials_or_settings() {
        assert_no_account_writes(|| {
            for input in [
                json!({"operation":"status"}),
                json!({"operation":"refresh"}),
                json!({"operation":"configure","revision":0,"order":[1,2],"visible":[2]}),
                json!({"operation":"begin","revision":0,"slot":1}),
                json!({"operation":"openLogin","loginId":"fixture"}),
                json!({"operation":"cancel","loginId":"fixture"}),
                json!({"operation":"remove","revision":0,"slot":1,"confirm":true}),
            ] {
                let result = tauri::async_runtime::block_on(codex_accounts_request(input));
                assert_eq!(result.unwrap_err(), "当前系统尚未启用 Codex 额度账号管理");
            }
        });
    }
}
