use super::{extract_zip_safe, publish_source, read_json_from_dir, ComponentLibraryEntry};
use std::{fs, path::Path};

pub(crate) fn import(
    home: &Path,
    component: &str,
    pet_name: Option<&str>,
    portrait: Option<Vec<u8>>,
) -> Result<ComponentLibraryEntry, String> {
    let bytes: &[u8] = match component {
        "book-pocket" => include_bytes!(
            "../../../tools/offline-components/packages/book-pocket-offline.hachiapp"
        ),
        "hachimi-learn" => include_bytes!(
            "../../../tools/offline-components/packages/hachimi-learn-offline.hachiapp"
        ),
        "cozy-pop-stars" => include_bytes!(
            "../../../tools/offline-components/packages/cozy-pop-stars-daily.hachiapp"
        ),
        "tomato-clock" => {
            include_bytes!("../../../tools/offline-components/packages/tomato-clock.hachiapp")
        }
        _ => return Err("此组件没有实验版".into()),
    };
    let tmp = tempfile::tempdir().map_err(|e| e.to_string())?;
    let package = tmp.path().join("edition.hachiapp");
    fs::write(&package, bytes).map_err(|e| e.to_string())?;
    if component != "tomato-clock" {
        return super::import_hachiapp(home, &package);
    }
    let name: String = pet_name
        .unwrap_or("")
        .trim()
        .chars()
        .filter(|c| !c.is_control())
        .take(6)
        .collect();
    if name.is_empty() {
        return Err("请先选择桌宠形象".into());
    }
    let source = tmp.path().join("timer");
    fs::create_dir(&source).map_err(|e| e.to_string())?;
    extract_zip_safe(&package, &source)?;
    let mut widget = read_json_from_dir(&source, "runtime/widget.json").ok_or("缺少番茄钟界面")?;
    let elements = widget["surface"]["elements"]
        .as_array_mut()
        .ok_or("缺少番茄钟控件")?;
    let message = elements
        .iter_mut()
        .find(|n| n["type"] == "text" && n["show"]["var"] == "pet_greeting")
        .ok_or("缺少宠物反馈控件")?;
    message["text"] = format!("{}：完成啦！", name).into();
    let image = source.join("assets/focus-pet.png");
    if let Some(rgba) = portrait {
        if rgba.len() != 96 * 96 * 4 {
            return Err("宠物缩图尺寸不符".into());
        }
        image::RgbaImage::from_raw(96, 96, rgba)
            .ok_or("缩图无效")?
            .save(image)
            .map_err(|e| e.to_string())?;
    } else {
        // A video-only pet can give a named greeting without another pet's picture.
        message["y"] = 235.into();
        elements.retain(|n| n["image"] != "pet");
        widget["surface"]["images"]
            .as_array_mut()
            .ok_or("缺少素材表")?
            .retain(|v| v["id"] != "pet");
        fs::remove_file(image).map_err(|e| e.to_string())?;
    }
    fs::write(
        source.join("runtime/widget.json"),
        serde_json::to_vec(&widget).map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    publish_source(home, &source)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device_files(
        entry: &ComponentLibraryEntry,
        caps: &serde_json::Value,
    ) -> Result<Vec<(String, Vec<u8>)>, String> {
        crate::usb_serial::UsbSerialManager::prepare_widget_delivery(
            &entry.id,
            Path::new(&entry.path),
            &Default::default(),
            None,
            Some(caps),
        )
    }

    #[test]
    fn import_offline_editions_through_real_library_and_personalize_focus() {
        let user = tempfile::tempdir().unwrap();
        let home = user.path();
        for (source, expected) in [
            ("book-pocket", "book-pocket-offline"),
            ("hachimi-learn", "hachimi-learn-offline"),
            ("cozy-pop-stars", "cozy-pop-stars-daily"),
        ] {
            let entry = import(home, source, None, None).unwrap();
            assert_eq!(entry.id, expected);
            assert!(entry.valid);
            assert!(device_files(&entry, &serde_json::json!({})).is_err());
            let mut caps = serde_json::json!({
                "widgetWasm": "hachi-wasm-v1", "widgetWasmCanvas": "640x480-v1"
            });
            if source == "cozy-pop-stars" {
                assert!(device_files(&entry, &caps).is_err());
                caps["widgetWasmHost"] = "hachi-host-v1".into();
            }
            let files = device_files(&entry, &caps).unwrap();
            assert!(files.iter().any(|(p, _)| p == "runtime/app.wasm"));
            assert!(!files.iter().any(|(p, _)| p.starts_with("pc/")));
        }
        let entry = import(home, "tomato-clock", Some("团子"), None).unwrap();
        assert_eq!(entry.id, "tomato-clock");
        let widget = read_json_from_dir(Path::new(&entry.path), "runtime/widget.json").unwrap();
        assert!(widget["surface"]["elements"]
            .as_array()
            .unwrap()
            .iter()
            .any(|n| n["text"] == "团子：完成啦！"));
        assert!(!Path::new(&entry.path).join("assets/focus-pet.png").exists());
        assert!(import(home, "tomato-clock", Some("团子"), Some(vec![0; 80])).is_err());
        let with_image = import(
            home,
            "tomato-clock",
            Some("团子"),
            Some(vec![123; 96 * 96 * 4]),
        )
        .unwrap();
        assert_ne!(entry.version_hash, with_image.version_hash);
        device_files(&with_image, &serde_json::json!({})).unwrap();
        assert!(Path::new(&with_image.path)
            .join("assets/focus-pet.png")
            .exists());
        assert!(Path::new(&entry.path).exists());
        assert!(import(home, "../anything", None, None).is_err());
    }
}
