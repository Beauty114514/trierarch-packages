//! PRoot argv and environment for the interactive shell.
//!
//! Derived directly from Trierarch-old's `android/proot/args.rs`, with the
//! old desktop-only mounts, host services and rootfs installer removed.

use super::ProotSpec;
use anyhow::{Context, Result};
use std::ffi::CString;

pub(super) fn build_exec_args(spec: &ProotSpec) -> Result<(Vec<CString>, Vec<CString>)> {
    let proot = spec.native_library_dir.join("libproot.so");
    let loader = spec.native_library_dir.join("libproot_loader.so");
    validate(spec, &proot, &loader)?;

    let rootfs = spec.rootfs.display();
    let argv = strings(&[
        proot.display().to_string(),
        "-r".into(),
        rootfs.to_string(),
        "-L".into(),
        "--link2symlink".into(),
        "--sysvipc".into(),
        "--kill-on-exit".into(),
        "--root-id".into(),
        "--bind=/dev".into(),
        "--bind=/proc".into(),
        "--bind=/sys".into(),
        format!("--bind={rootfs}/tmp:/dev/shm"),
        "--bind=/dev/urandom:/dev/random".into(),
        "--bind=/proc/self/fd:/dev/fd".into(),
        "--bind=/proc/self/fd/0:/dev/stdin".into(),
        "--bind=/proc/self/fd/1:/dev/stdout".into(),
        "--bind=/proc/self/fd/2:/dev/stderr".into(),
        format!("--bind={rootfs}/proc/.loadavg:/proc/loadavg"),
        format!("--bind={rootfs}/proc/.stat:/proc/stat"),
        format!("--bind={rootfs}/proc/.uptime:/proc/uptime"),
        format!("--bind={rootfs}/proc/.version:/proc/version"),
        format!("--bind={rootfs}/proc/.vmstat:/proc/vmstat"),
        format!("--bind={rootfs}/proc/.sysctl_entry_cap_last_cap:/proc/sys/kernel/cap_last_cap"),
        format!("--bind={rootfs}/proc/.sysctl_inotify_max_user_watches:/proc/sys/fs/inotify/max_user_watches"),
        format!("--bind={rootfs}/sys/.empty:/sys/fs/selinux"),
        spec.shell.display().to_string(),
        "-i".into(),
    ])?;
    let env = strings(&[
        format!("PROOT_LOADER={}", loader.display()),
        format!("PROOT_TMP_DIR={}", spec.cache_dir.display()),
        "HOME=/root".into(),
        "PS1=[\\u@\\h \\W]\\$ ".into(),
        "TERM=xterm-256color".into(),
        "LANG=C.UTF-8".into(),
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/games:/usr/games:/system/bin:/system/xbin".into(),
        "TMPDIR=/tmp".into(),
        "USER=root".into(),
        "LOGNAME=root".into(),
    ])?;
    Ok((argv, env))
}

fn validate(spec: &ProotSpec, proot: &std::path::Path, loader: &std::path::Path) -> Result<()> {
    let guest_shell = spec.shell.to_string_lossy();
    anyhow::ensure!(
        spec.shell.is_absolute(),
        "shell must be an absolute path inside the rootfs"
    );
    let shell_relative = spec
        .shell
        .strip_prefix("/")
        .context("shell must be an absolute path inside the rootfs")?;
    anyhow::ensure!(
        spec.rootfs.join(shell_relative).is_file(),
        "rootfs does not contain configured shell: {guest_shell}"
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
        anyhow::ensure!(
            spec.rootfs.join(path).is_file(),
            "rootfs is missing PRoot compatibility file: {path}"
        );
    }
    anyhow::ensure!(
        spec.rootfs.join("sys/.empty").is_dir(),
        "rootfs is missing PRoot compatibility directory: sys/.empty"
    );
    anyhow::ensure!(proot.is_file(), "installed libproot.so is missing");
    anyhow::ensure!(loader.is_file(), "installed libproot_loader.so is missing");
    anyhow::ensure!(spec.cache_dir.is_dir(), "PRoot cache directory does not exist");
    Ok(())
}

fn strings(values: &[String]) -> Result<Vec<CString>> {
    values
        .iter()
        .map(|value| CString::new(value.as_bytes()).context("PRoot argv/environment contains NUL"))
        .collect()
}
