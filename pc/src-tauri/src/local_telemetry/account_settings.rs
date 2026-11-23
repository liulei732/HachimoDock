//! PC-only display settings. Credentials are not part of this schema.
use serde::{Deserialize, Serialize};
use std::{fs, io::Write, path::Path};

pub const MAX_ACCOUNTS: u8 = 8;
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Settings {
    pub version: u8,
    pub revision: u64,
    pub order: Vec<u8>,
    pub visible: Vec<u8>,
}
impl Default for Settings {
    fn default() -> Self {
        // Preserve existing two-slot packages until the user manages the list.
        Self {
            version: 1,
            revision: 0,
            order: vec![1, 2],
            visible: vec![1, 2],
        }
    }
}
impl Settings {
    pub fn validate(&self) -> Result<(), String> {
        let unique = |v: &[u8]| {
            v.iter()
                .enumerate()
                .all(|(i, s)| (1..=MAX_ACCOUNTS).contains(s) && !v[..i].contains(s))
        };
        if self.version != 1
            || self.order.len() > MAX_ACCOUNTS as usize
            || !unique(&self.order)
            || !unique(&self.visible)
            || self.visible.iter().any(|s| !self.order.contains(s))
        {
            return Err("账号列表设置无效，原设置保留".into());
        }
        Ok(())
    }
    pub fn displayed(&self) -> Vec<u8> {
        self.order
            .iter()
            .copied()
            .filter(|s| self.visible.contains(s))
            .collect()
    }
    pub fn selected(&self, previous: u8, direction: i8) -> Option<u8> {
        let shown = self.displayed();
        if shown.is_empty() {
            return None;
        }
        let Some(i) = shown.iter().position(|s| *s == previous) else {
            return shown.first().copied();
        };
        let next = (i as isize + direction.signum() as isize).rem_euclid(shown.len() as isize);
        Some(shown[next as usize])
    }
    pub fn replace_display(
        &mut self,
        revision: u64,
        order: Vec<u8>,
        visible: Vec<u8>,
    ) -> Result<(), String> {
        if self.revision != revision {
            return Err("账号列表已更新，请刷新后重试".into());
        }
        // Configuration cannot register, replace, or forget a login.
        if order.len() != self.order.len() || order.iter().any(|s| !self.order.contains(s)) {
            return Err("账号列表已变更，请刷新后重试".into());
        }
        let next = Self {
            version: 1,
            revision: self.revision.checked_add(1).ok_or("设置修订已达上限")?,
            order,
            visible,
        };
        next.validate()?;
        *self = next;
        Ok(())
    }
}
pub fn protected_directory(root: &Path) -> Result<(), String> {
    #[cfg(windows)]
    {
        let _ = root;
        return Err("Windows 独立账号存储尚未启用".into());
    }
    if let Ok(m) = fs::symlink_metadata(root) {
        if !m.is_dir() || m.file_type().is_symlink() {
            return Err("账号目录不可用".into());
        }
    } else {
        fs::create_dir_all(root).map_err(|_| "无法创建账号目录")?;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if fs::metadata(root).map_err(|_| "账号目录不可用")?.uid() != unsafe { libc::geteuid() }
        {
            return Err("账号目录不属于当前用户".into());
        }
        fs::set_permissions(root, fs::Permissions::from_mode(0o700))
            .map_err(|_| "无法保护账号目录")?;
    }
    Ok(())
}
pub fn load(root: &Path) -> Result<Settings, String> {
    let path = root.join("display-settings.json");
    match fs::symlink_metadata(&path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Settings::default()),
        Err(_) => Err("无法读取账号设置".into()),
        Ok(m) => {
            if !m.is_file() || m.file_type().is_symlink() || m.len() > 4096 {
                return Err("账号设置无效，原数据保留".into());
            }
            let v: Settings =
                serde_json::from_slice(&fs::read(path).map_err(|_| "无法读取账号设置")?)
                    .map_err(|_| "账号设置无效，原数据保留")?;
            v.validate()?;
            Ok(v)
        }
    }
}
pub fn save(root: &Path, settings: &Settings) -> Result<(), String> {
    settings.validate()?;
    protected_directory(root)?;
    let mut f = tempfile::NamedTempFile::new_in(root).map_err(|_| "无法准备账号设置")?;
    f.write_all(&serde_json::to_vec(settings).map_err(|_| "账号设置无效")?)
        .and_then(|_| f.as_file().sync_all())
        .map_err(|_| "无法保存账号设置")?;
    f.persist(root.join("display-settings.json"))
        .map_err(|_| "无法保存账号设置")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn display_order_skips_hidden_and_reconciles_a_removed_selection() {
        let mut s = Settings::default();
        s.replace_display(0, vec![2, 1], vec![2]).unwrap();
        assert_eq!(s.displayed(), vec![2]);
        assert_eq!(s.selected(1, 1), Some(2));
        assert_eq!(s.selected(2, -1), Some(2));
        s.replace_display(1, vec![1, 2], vec![]).unwrap();
        assert_eq!(s.selected(2, 1), None);
    }
    #[test]
    fn stale_and_invalid_edits_leave_original_unchanged() {
        let mut s = Settings::default();
        let old = s.clone();
        for (r, o, v) in [
            (1, vec![1, 2], vec![1]),
            (0, vec![1, 1], vec![1]),
            (0, vec![1, 3], vec![1]),
            (0, vec![1, 2], vec![3]),
        ] {
            assert!(s.replace_display(r, o, v).is_err());
            assert_eq!(s, old);
        }
    }
    #[cfg(unix)]
    #[test]
    fn settings_round_trip_without_credentials_and_reject_corrupt_state() {
        let t = tempfile::tempdir().unwrap();
        let mut s = Settings::default();
        s.replace_display(0, vec![2, 1], vec![1, 2]).unwrap();
        save(t.path(), &s).unwrap();
        assert_eq!(load(t.path()).unwrap(), s);
        let text = fs::read_to_string(t.path().join("display-settings.json")).unwrap();
        assert!(!text.contains("token"));
        fs::write(t.path().join("display-settings.json"), "{}").unwrap();
        assert!(load(t.path()).is_err());
    }
    #[test]
    fn rejects_duplicates_unknown_fields_and_out_of_range_slots() {
        for text in [
            r#"{"version":1,"revision":0,"order":[1,1],"visible":[1]}"#,
            r#"{"version":1,"revision":0,"order":[9],"visible":[9]}"#,
            r#"{"version":1,"revision":0,"order":[1],"visible":[1],"auth":{}}"#,
        ] {
            assert!(
                serde_json::from_str::<Settings>(text)
                    .map(|s| s.validate().is_ok())
                    .unwrap_or(false)
                    == false
            );
        }
    }
    #[cfg(unix)]
    #[test]
    fn rejects_symlink_settings_and_preserves_private_permissions() {
        use std::os::unix::fs::{symlink, PermissionsExt};
        let t = tempfile::tempdir().unwrap();
        save(t.path(), &Settings::default()).unwrap();
        assert_eq!(
            fs::metadata(t.path().join("display-settings.json"))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
        fs::remove_file(t.path().join("display-settings.json")).unwrap();
        fs::write(t.path().join("other"), "{}").unwrap();
        symlink(
            t.path().join("other"),
            t.path().join("display-settings.json"),
        )
        .unwrap();
        assert!(load(t.path()).is_err());
    }
}
