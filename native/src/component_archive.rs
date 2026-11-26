//! Generic extraction for file components.
//!
//! A file component is a tar archive (or a plain file) that is materialized
//! under a rootfs. The extractor knows nothing about component names or
//! destinations; the caller owns the target directory.

use anyhow::{bail, Context, Result};
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

pub fn extract(archive: &Path, destination: &Path) -> Result<()> {
    anyhow::ensure!(archive.is_file(), "component archive does not exist: {}", archive.display());
    fs::create_dir_all(destination)
        .with_context(|| format!("create component destination {}", destination.display()))?;
    let name = archive.to_string_lossy();
    if name.ends_with(".tar.zst") {
        let file = fs::File::open(archive)?;
        let decoder = zstd::stream::read::Decoder::new(file)?;
        extract_tar(tar::Archive::new(decoder), destination)
    } else if name.ends_with(".tar.xz") {
        let file = fs::File::open(archive)?;
        let decoder = xz2::read::XzDecoder::new(file);
        extract_tar(tar::Archive::new(decoder), destination)
    } else {
        bail!("unsupported component archive format: {}", archive.display())
    }
}

fn safe_relative(path: &Path) -> Result<PathBuf> {
    if path.is_absolute() {
        bail!("archive entry is absolute: {}", path.display());
    }
    let mut clean = PathBuf::new();
    for part in path.components() {
        match part {
            Component::Normal(value) => clean.push(value),
            Component::CurDir => {}
            _ => bail!("archive entry escapes destination: {}", path.display()),
        }
    }
    Ok(clean)
}

fn extract_tar<R: io::Read>(mut archive: tar::Archive<R>, destination: &Path) -> Result<()> {
    for entry in archive.entries().context("read component archive")? {
        let mut entry = entry.context("read component archive entry")?;
        let relative = safe_relative(&entry.path().context("read component entry path")?)?;
        if relative.as_os_str().is_empty() { continue; }
        let target = destination.join(&relative);
        let kind = entry.header().entry_type();
        if kind.is_dir() {
            fs::create_dir_all(&target)?;
            continue;
        }
        if let Some(parent) = target.parent() { fs::create_dir_all(parent)?; }
        if kind.is_symlink() {
            let link = entry.link_name()?.context("component symlink has no target")?;
            if link.is_absolute() || link.components().any(|part| matches!(part, Component::ParentDir)) {
                bail!("component symlink escapes destination: {}", link.display());
            }
            let _ = fs::remove_file(&target);
            std::os::unix::fs::symlink(link, &target)?;
        } else if kind.is_hard_link() {
            let link = entry.link_name()?.context("component hard link has no target")?;
            let link = safe_relative(&link)?;
            fs::hard_link(destination.join(link), &target)?;
        } else if kind.is_file() {
            let mut output = fs::File::create(&target)?;
            io::copy(&mut entry, &mut output)?;
            use std::os::unix::fs::PermissionsExt;
            if let Ok(mode) = entry.header().mode() {
                let _ = fs::set_permissions(&target, fs::Permissions::from_mode(mode));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_a_zstd_tar_archive() {
        let dir = std::env::temp_dir().join(format!("trierarch-component-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let archive = dir.join("component.tar.zst");
        let mut tar_bytes = Vec::new();
        {
            let mut builder = tar::Builder::new(&mut tar_bytes);
            let mut header = tar::Header::new_gnu();
            header.set_size(4);
            header.set_mode(0o644);
            header.set_cksum();
            builder.append_data(&mut header, "component/lib.so", &b"test"[..]).unwrap();
            builder.finish().unwrap();
        }
        fs::write(&archive, zstd::stream::encode_all(&tar_bytes[..], 3).unwrap()).unwrap();
        let out = dir.join("out");
        extract(&archive, &out).unwrap();
        assert_eq!(fs::read(out.join("component/lib.so")).unwrap(), b"test");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn rejects_escaping_paths() {
        assert!(safe_relative(Path::new("../outside")).is_err());
        assert!(safe_relative(Path::new("/absolute")).is_err());
        assert_eq!(safe_relative(Path::new("ok/file.so")).unwrap(), Path::new("ok/file.so"));
    }
}
