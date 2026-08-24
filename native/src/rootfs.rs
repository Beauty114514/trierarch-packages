//! Import trusted `.tar.xz` Linux rootfs archives into Trierarch's fixed rootfs store.

use anyhow::{bail, ensure, Context, Result};
use std::fs::{self, File};
use std::io::BufReader;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const ROOTFS_DIRECTORY: &str = "rootfs";

/// Imports an xz-compressed tar archive as `files/rootfs/<name>`.
///
/// The destination is never replaced: callers receive an error when `name`
/// already exists. Archives may contain either a rootfs directly or exactly one
/// wrapper directory around the rootfs.
pub fn import_xz_tar(archive_path: &Path, files_directory: &Path, name: &str) -> Result<PathBuf> {
    validate_name(name)?;
    ensure!(
        archive_path.is_file(),
        "archive does not exist: {}",
        archive_path.display()
    );
    ensure!(
        archive_path
            .extension()
            .is_some_and(|extension| extension.eq_ignore_ascii_case("xz")),
        "only .tar.xz rootfs archives are supported"
    );
    ensure!(
        files_directory.is_dir(),
        "Trierarch files directory does not exist"
    );

    let rootfs_directory = files_directory.join(ROOTFS_DIRECTORY);
    fs::create_dir_all(&rootfs_directory).context("create rootfs directory")?;
    let destination = rootfs_directory.join(name);
    ensure!(
        !destination.exists(),
        "rootfs name already exists: {name}; choose a different --name"
    );

    let staging = rootfs_directory.join(format!(".{name}.import-{}", unique_suffix()));
    let result = import_into_staging(archive_path, &staging, &destination);
    if result.is_err() {
        make_tree_writable(&staging);
        let _ = fs::remove_dir_all(&staging);
    }
    result.map(|()| destination)
}

fn import_into_staging(archive_path: &Path, staging: &Path, destination: &Path) -> Result<()> {
    let payload = staging.join("payload");
    fs::create_dir_all(&payload).context("create rootfs import staging directory")?;
    let directory_modes = extract_xz_tar(archive_path, &payload)?;

    let rootfs = select_rootfs(&payload)?;
    setup_proot_compatibility(&rootfs)?;
    validate_rootfs(&rootfs)?;
    restore_directory_permissions(directory_modes)?;

    fs::rename(&rootfs, destination).with_context(|| {
        format!(
            "install rootfs at {} (the name may have been created concurrently)",
            destination.display()
        )
    })?;
    let _ = fs::remove_dir_all(staging);
    Ok(())
}

fn extract_xz_tar(archive_file: &Path, destination: &Path) -> Result<Vec<(PathBuf, u32)>> {
    let mut directory_modes = Vec::new();
    with_xz_archive(archive_file, |archive| {
        for entry in archive.entries().context("read rootfs archive")? {
            let entry = entry.context("read rootfs archive entry")?;
            let Some(path) =
                safe_archive_relative(&entry.path().context("read rootfs archive entry path")?)?
            else {
                continue;
            };
            let target = destination.join(&path);
            if entry.header().entry_type().is_dir() {
                fs::create_dir_all(&target)
                    .with_context(|| format!("create extracted directory {}", target.display()))?;
                directory_modes.push((
                    target,
                    entry.header().mode().context("read directory mode")?,
                ));
            } else if let Some(parent) = target.parent() {
                fs::create_dir_all(parent)
                    .with_context(|| format!("create extracted directory {}", parent.display()))?;
            }
        }
        Ok(())
    })?;

    with_xz_archive(archive_file, |archive| {
        for entry in archive.entries().context("read rootfs archive")? {
            let mut entry = entry.context("read rootfs archive entry")?;
            let Some(path) =
                safe_archive_relative(&entry.path().context("read rootfs archive entry path")?)?
            else {
                continue;
            };
            if entry.header().entry_type().is_dir() {
                continue;
            }
            if entry.header().entry_type().is_hard_link() {
                copy_hard_link_as_file(&entry, destination, &path)?;
                continue;
            }
            ensure!(
                entry
                    .unpack_in(destination)
                    .with_context(|| format!("extract rootfs archive entry {}", path.display()))?,
                "rootfs archive entry escapes its destination"
            );
        }
        Ok(())
    })?;

    Ok(directory_modes)
}

/// Android app-data directories intentionally deny `link(2)`. A rootfs does
/// not need hard-linked files to share an inode, so materialize them as copies.
fn copy_hard_link_as_file<R: std::io::Read>(
    entry: &tar::Entry<'_, R>,
    destination: &Path,
    path: &Path,
) -> Result<()> {
    let link_name = entry
        .link_name()
        .context("read rootfs hard-link target")?
        .context("rootfs hard link has no target")?;
    let Some(link_path) = safe_archive_relative(&link_name)? else {
        bail!("rootfs hard link has an empty target");
    };
    let source = destination.join(link_path);
    let target = destination.join(path);
    ensure!(
        source.is_file(),
        "rootfs hard-link target is unavailable: {}",
        source.display()
    );
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create extracted directory {}", parent.display()))?;
    }
    fs::copy(&source, &target).with_context(|| {
        format!(
            "copy rootfs hard link {} from {}",
            target.display(),
            source.display()
        )
    })?;
    fs::set_permissions(
        &target,
        fs::Permissions::from_mode(entry.header().mode().context("read hard-link mode")?),
    )
    .with_context(|| format!("set permissions for copied hard link {}", target.display()))?;
    Ok(())
}

fn restore_directory_permissions(directory_modes: Vec<(PathBuf, u32)>) -> Result<()> {
    for (directory, mode) in directory_modes {
        fs::set_permissions(&directory, fs::Permissions::from_mode(mode)).with_context(|| {
            format!("restore directory permissions for {}", directory.display())
        })?;
    }
    Ok(())
}

fn make_tree_writable(root: &Path) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if fs::symlink_metadata(&path)
            .map(|metadata| metadata.file_type().is_dir())
            .unwrap_or(false)
        {
            make_tree_writable(&path);
        }
        let _ = fs::set_permissions(&path, fs::Permissions::from_mode(0o755));
    }
    let _ = fs::set_permissions(root, fs::Permissions::from_mode(0o755));
}

fn with_xz_archive<T>(
    archive_path: &Path,
    action: impl FnOnce(&mut tar::Archive<xz2::read::XzDecoder<BufReader<File>>>) -> Result<T>,
) -> Result<T> {
    let archive_file = File::open(archive_path).context("open rootfs archive")?;
    let decoder = xz2::read::XzDecoder::new(BufReader::new(archive_file));
    action(&mut tar::Archive::new(decoder))
}

fn safe_archive_relative(path: &Path) -> Result<Option<PathBuf>> {
    let mut clean = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::Normal(segment) => clean.push(segment),
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                bail!("rootfs archive contains an unsafe path: {}", path.display());
            }
        }
    }
    if clean.as_os_str().is_empty() {
        Ok(None)
    } else {
        Ok(Some(clean))
    }
}

fn select_rootfs(payload: &Path) -> Result<PathBuf> {
    if looks_like_rootfs(payload) {
        return Ok(payload.to_path_buf());
    }
    let entries: Vec<_> = fs::read_dir(payload)
        .context("list extracted rootfs")?
        .collect::<std::result::Result<Vec<_>, _>>()
        .context("list extracted rootfs")?;
    ensure!(
        entries.len() == 1 && entries[0].path().is_dir() && looks_like_rootfs(&entries[0].path()),
        "archive must contain a rootfs directly or inside one top-level directory"
    );
    Ok(entries[0].path())
}

fn looks_like_rootfs(path: &Path) -> bool {
    path.join("etc/os-release").is_file()
        && (path.join("bin/sh").exists() || path.join("usr/bin/sh").exists())
}

fn setup_proot_compatibility(rootfs: &Path) -> Result<()> {
    fs::create_dir_all(rootfs.join("proc")).context("create rootfs proc directory")?;
    fs::create_dir_all(rootfs.join("sys/.empty")).context("create rootfs sys directory")?;
    for (relative, contents) in [
        ("proc/.loadavg", "0.12 0.07 0.02 2/165 765\n"),
        (
            "proc/.stat",
            "cpu 1957 0 2877 93280 262 342 254 87 0 0\ncpu0 31 0 226 12027 82 10 4 9 0 0\n",
        ),
        ("proc/.uptime", "124.08 932.80\n"),
        (
            "proc/.version",
            "Linux version 6.2.1 (proot@trierarch) #1 SMP PREEMPT_DYNAMIC\n",
        ),
        (
            "proc/.vmstat",
            "nr_free_pages 1743136\nnr_zone_inactive_anon 179281\n",
        ),
        ("proc/.sysctl_entry_cap_last_cap", "40\n"),
        ("proc/.sysctl_inotify_max_user_watches", "4096\n"),
    ] {
        let path = rootfs.join(relative);
        if !path.exists() {
            fs::write(&path, contents).with_context(|| format!("write {}", path.display()))?;
        }
    }
    Ok(())
}

fn validate_rootfs(rootfs: &Path) -> Result<()> {
    ensure!(
        looks_like_rootfs(rootfs),
        "rootfs is missing etc/os-release or bin/sh"
    );
    for path in [
        "proc/.loadavg",
        "proc/.stat",
        "proc/.uptime",
        "proc/.version",
        "proc/.vmstat",
        "proc/.sysctl_entry_cap_last_cap",
        "proc/.sysctl_inotify_max_user_watches",
    ] {
        ensure!(
            rootfs.join(path).is_file(),
            "rootfs is missing PRoot compatibility file: {path}"
        );
    }
    ensure!(
        rootfs.join("sys/.empty").is_dir(),
        "rootfs is missing PRoot compatibility directory"
    );
    Ok(())
}

fn validate_name(name: &str) -> Result<()> {
    let valid = !name.is_empty()
        && name.len() <= 64
        && name.as_bytes()[0].is_ascii_lowercase()
        && name.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-' || byte == b'_'
        });
    if !valid {
        bail!("--name must use lowercase English letters, digits, '-' or '_', and start with a letter");
    }
    Ok(())
}

fn unique_suffix() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos()
}
