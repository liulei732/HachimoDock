//! Keep native user storage aligned with the platform's bundled launchers.
use std::{env, ffi::OsStr, path::PathBuf};

fn resolve(windows: bool, home: Option<&OsStr>, profile: Option<&OsStr>) -> Option<PathBuf> {
    // Git Bash can set HOME to a different directory. The Windows launcher and
    // Python use USERPROFILE, so native installation must use the same root.
    (if windows { profile } else { home })
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
}

pub(crate) fn current() -> Option<PathBuf> {
    resolve(
        cfg!(windows),
        env::var_os("HOME").as_deref(),
        env::var_os("USERPROFILE").as_deref(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_uses_profile_even_when_git_home_is_different() {
        let profile = OsStr::new(r"C:\Users\Fixture User");
        assert_eq!(
            resolve(true, Some(OsStr::new(r"D:\Git Home")), Some(profile)),
            Some(PathBuf::from(profile))
        );
    }

    #[test]
    fn windows_preserves_unicode_spaces_and_each_current_user() {
        for name in [r"C:\Users\用户 甲", r"D:\Profiles\Second User"] {
            assert_eq!(
                resolve(true, None, Some(OsStr::new(name))),
                Some(PathBuf::from(name))
            );
        }
    }

    #[test]
    fn missing_profile_does_not_guess_from_home_or_working_directory() {
        for profile in [None, Some(OsStr::new(""))] {
            assert_eq!(resolve(true, Some(OsStr::new("other-home")), profile), None);
        }
    }

    #[test]
    fn unix_uses_home() {
        assert_eq!(
            resolve(
                false,
                Some(OsStr::new("unix-home")),
                Some(OsStr::new("profile"))
            ),
            Some(PathBuf::from("unix-home"))
        );
        assert_eq!(resolve(false, None, Some(OsStr::new("profile"))), None);
    }
}
