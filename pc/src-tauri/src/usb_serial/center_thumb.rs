// The launcher reads 16 little-endian RGB565 palette entries followed by two
// palette indices per byte (left pixel in the high nibble), 64 x 48 pixels.
use image::{imageops::FilterType, ImageDecoder};
use sha2::{Digest, Sha256};
use std::path::Path;

pub(crate) const PATH: &str = "runtime/center-thumb.p4t";
pub(crate) const MATCH: &str = "runtime/center-thumb.match";
pub(crate) const PROFILE: &str = "p4t-64x48-v1";

pub(crate) fn append(dir: &Path, files: &mut Vec<(String, Vec<u8>)>) -> Result<(), String> {
    let path = dir.join(crate::clawpkg::HACHI_COVER_PATH);
    if !path.try_exists().map_err(|e| e.to_string())? {
        return Ok(());
    }
    let bytes = crate::clawpkg::read_clawpkg_source_file(&path, crate::clawpkg::HACHI_COVER_MAX)?;
    let decoder = image::codecs::png::PngDecoder::new(std::io::Cursor::new(bytes))
        .map_err(|e| e.to_string())?;
    if !matches!(decoder.dimensions(), (320, 240) | (640, 480)) {
        return Err("组件封面尺寸无效".into());
    }
    let image = image::DynamicImage::from_decoder(decoder).map_err(|e| e.to_string())?;
    files.push((PATH.into(), encode(&image)));
    Ok(())
}

// Legacy community covers are cached by the installed content hash outside
// the package. Resolve exactly the same library entry as the PC card.
pub(crate) fn append_cached(
    home: &Path,
    source: &Path,
    id: &str,
    files: &mut Vec<(String, Vec<u8>)>,
) -> Result<(), String> {
    if files.iter().any(|(name, _)| name == PATH) {
        return Ok(());
    }
    let Some(hash) = source.file_name().and_then(|v| v.to_str()) else {
        return Ok(());
    };
    let library = crate::component_library::library_root(home);
    if hash.len() != 16
        || !hash.bytes().all(|b| b.is_ascii_hexdigit())
        || source != library.join(id).join(hash)
        || !crate::component_extension::valid_id(id)
    {
        return Ok(());
    }
    let cached = library
        .parent()
        .unwrap()
        .join("community/covers")
        .join(format!("{id}-{hash}.jpg"));
    if !cached.try_exists().map_err(|e| e.to_string())? {
        return Ok(());
    }
    let bytes = crate::clawpkg::read_clawpkg_source_file(&cached, 2 * 1024 * 1024)?;
    let mut reader =
        image::ImageReader::with_format(std::io::Cursor::new(bytes), image::ImageFormat::Jpeg);
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(2048);
    limits.max_image_height = Some(2048);
    limits.max_alloc = Some(24 * 1024 * 1024);
    reader.limits(limits);
    let image = reader.decode().map_err(|e| e.to_string())?;
    files.push((PATH.into(), encode(&image)));
    Ok(())
}

pub(crate) fn append_source(
    dir: &Path,
    source: &Path,
    id: &str,
    files: &mut Vec<(String, Vec<u8>)>,
) -> Result<(), String> {
    append(dir, files)?;
    if !files.iter().any(|(name, _)| name == PATH) {
        if let Ok(home) = crate::get_home_dir() {
            append_cached(&home, source, id, files)?;
        }
    }
    Ok(())
}

pub(crate) fn encode(image: &image::DynamicImage) -> Vec<u8> {
    let rgba = image.resize_exact(64, 48, FilterType::Triangle).to_rgba8();
    let pixels: Vec<[u8; 3]> = rgba
        .pixels()
        .map(|p| {
            let alpha = u16::from(p[3]);
            std::array::from_fn(|i| {
                ((u16::from(p[i]) * alpha + 255 * (255 - alpha) + 127) / 255) as u8
            })
        })
        .collect();
    pack(&pixels)
}

// Median-cut over the bounded thumbnail. Keep original pixel positions through
// the splits so each palette index is assigned once, without a second search.
fn pack(pixels: &[[u8; 3]]) -> Vec<u8> {
    let mut groups = vec![(0..pixels.len()).collect::<Vec<_>>()];
    while groups.len() < 16 {
        let split = groups
            .iter()
            .enumerate()
            .filter_map(|(i, group)| {
                let mut lo = [255; 3];
                let mut hi = [0; 3];
                for &p in group {
                    for c in 0..3 {
                        lo[c] = lo[c].min(pixels[p][c]);
                        hi[c] = hi[c].max(pixels[p][c]);
                    }
                }
                let axis = (0..3).max_by_key(|&c| hi[c] - lo[c]).unwrap();
                (hi[axis] > lo[axis]).then_some((
                    (hi[axis] - lo[axis]) as usize * group.len(),
                    i,
                    axis,
                ))
            })
            .max();
        let Some((_, i, axis)) = split else { break };
        groups[i].sort_by_key(|&p| pixels[p][axis]);
        let mid = groups[i].len() / 2;
        let right = groups[i].split_off(mid);
        groups.push(right);
    }
    let mut out = vec![0u8; 1568];
    for (index, group) in groups.iter().enumerate() {
        let mut sums = [0usize; 3];
        for &p in group {
            for c in 0..3 {
                sums[c] += usize::from(pixels[p][c]);
            }
            out[32 + p / 2] |= (index as u8) << (if p % 2 == 0 { 4 } else { 0 });
        }
        let [r, g, b] = sums.map(|n| (n / group.len()) as u16);
        out[index * 2..index * 2 + 2]
            .copy_from_slice(&((r >> 3) << 11 | (g >> 2) << 5 | b >> 3).to_le_bytes());
    }
    out
}

pub(crate) fn repair_files(
    dir: &Path,
    source: &Path,
    id: &str,
) -> Result<Vec<(String, Vec<u8>)>, String> {
    let raw =
        crate::clawpkg::read_clawpkg_source_file(&dir.join("runtime/widget.json"), 512 * 1024)?;
    let widget = super::prepare_p4_widget_file(id, "runtime/widget.json", &raw)?;
    let mut files = vec![(MATCH.into(), Sha256::digest(widget).to_vec())];
    append_source(dir, source, id, &mut files)?;
    if files.len() != 2 {
        return Err("此组件没有可同步的 PC 封面".into());
    }
    Ok(files)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn center_thumb_palette_and_pixel_order_match_the_launcher() {
        let pixels: Vec<_> = (0..3072)
            .map(|p| if p % 2 == 0 { [255, 0, 0] } else { [0, 0, 255] })
            .collect();
        let encoded = pack(&pixels);
        assert_eq!(encoded.len(), 1568);
        for (pair, &byte) in encoded[32..].iter().enumerate() {
            for (side, index) in [byte >> 4, byte & 15].into_iter().enumerate() {
                let i = usize::from(index) * 2;
                assert_eq!(
                    u16::from_le_bytes([encoded[i], encoded[i + 1]]),
                    if (pair * 2 + side) % 2 == 0 {
                        0xf800
                    } else {
                        0x001f
                    }
                );
            }
        }
    }
    #[test]
    fn center_thumb_cache_is_scoped_to_the_installed_id_and_content_hash() {
        let temp = tempfile::tempdir().unwrap();
        let library = crate::component_library::library_root(temp.path());
        let id = "fixture";
        let version = library.join(id).join("1234567890abcdef");
        let covers = library.parent().unwrap().join("community/covers");
        std::fs::create_dir_all(&covers).unwrap();
        let image = image::RgbImage::from_pixel(320, 240, image::Rgb([10, 90, 20]));
        image
            .save(covers.join("fixture-1234567890abcdef.jpg"))
            .unwrap();
        let mut files = Vec::new();
        append_cached(temp.path(), &version, "other", &mut files).unwrap();
        append_cached(
            temp.path(),
            &library.join(id).join("1234567890abcdee"),
            id,
            &mut files,
        )
        .unwrap();
        assert!(files.is_empty());
        append_cached(temp.path(), &version, id, &mut files).unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].1.len(), 1568);
        let before = files.clone();
        append_cached(temp.path(), &version, id, &mut files).unwrap();
        assert_eq!(files, before);
    }

    #[test]
    fn center_thumb_delivery_and_repair_use_the_same_cover_and_exact_runtime() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/block-combo");
        let id = "block-combo";
        let prepare = |caps| {
            crate::usb_serial::UsbSerialManager::prepare_widget_delivery(
                id,
                &root,
                &Default::default(),
                None,
                Some(&caps),
            )
            .unwrap()
        };
        let legacy = prepare(serde_json::json!({}));
        assert!(!legacy.iter().any(|(p, _)| p == PATH));
        let full = prepare(serde_json::json!({"widgetCenterThumb":PROFILE}));
        let repair = repair_files(&root, &root, id).unwrap();
        assert_eq!(repair.len(), 2);
        assert_eq!(repair[1], *full.iter().find(|(p, _)| p == PATH).unwrap());
        assert_eq!(repair[0].0, MATCH);
        assert_eq!(
            repair[0].1,
            Sha256::digest(
                &full
                    .iter()
                    .find(|(p, _)| p == "runtime/widget.json")
                    .unwrap()
                    .1
            )
            .to_vec()
        );
        assert_eq!(repair.iter().map(|(_, v)| v.len()).sum::<usize>(), 1600);
        assert!(!full.iter().any(|(p, _)| p.starts_with("pc/") || p == MATCH));
    }
}
