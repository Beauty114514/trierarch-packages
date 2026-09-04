//! PRoot argv and environment for the interactive shell.
//!
//! Derived directly from Trierarch-old's `android/proot/args.rs`, with the
//! old desktop-only mounts, host services and rootfs installer removed.

use super::ProotSpec;
use anyhow::{Context, Result};
use std::ffi::CString;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};

const WAYLAND_SOCKET: &str = "wayland-trierarch";
const GUEST_WAYLAND_RUNTIME_DIRECTORY: &str = "/tmp/trierarch-wayland-user";
const VIRGL_SOCKET: &str = "vtest.sock";
const GUEST_VIRGL_RUNTIME_DIRECTORY: &str = "/tmp/trierarch-virgl-host";
const GUEST_UDEV_COMPATIBILITY_LIBRARY: &str = "/opt/trierarch/compat/libtrierarch-udev-compat.so";
const GUEST_KWIN_WRAPPER: &str = "/opt/trierarch/compat/kwin-wayland-wrapper";

pub(super) fn build_exec_args(spec: &ProotSpec) -> Result<(Vec<CString>, Vec<CString>)> {
    let proot = spec.native_library_dir.join("libproot.so");
    let loader = spec.native_library_dir.join("libproot_loader.so");
    validate(spec, &proot, &loader)?;

    let rootfs = spec.rootfs.display();
    let mut argv = vec![
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
        format!(
            "--bind={rootfs}/proc/.sysctl_inotify_max_user_watches:/proc/sys/fs/inotify/max_user_watches"
        ),
        format!("--bind={rootfs}/sys/.empty:/sys/fs/selinux"),
    ];
    let x11 = !spec.x11_socket_directory.as_os_str().is_empty();
    let wayland = !spec.wayland_runtime_directory.as_os_str().is_empty();
    let virgl = !spec.virgl_runtime_directory.as_os_str().is_empty();
    let udev_compatibility = prepare_udev_compatibility_library(spec)?;
    if x11 {
        let host_socket = spec.x11_socket_directory.join("X0");
        let guest_directory = spec.rootfs.join("tmp/.X11-unix");
        std::fs::create_dir_all(&guest_directory).with_context(|| {
            format!("create guest X11 directory: {}", guest_directory.display())
        })?;
        let guest_socket = guest_directory.join("X0");
        if !guest_socket.exists() {
            std::fs::File::create(&guest_socket).with_context(|| {
                format!(
                    "create guest X11 socket mountpoint: {}",
                    guest_socket.display()
                )
            })?;
        }
        argv.push(format!(
            "--bind={}:/tmp/.X11-unix/X0",
            host_socket.display()
        ));
    }
    if wayland {
        bind_socket(
            &mut argv,
            &spec.rootfs,
            &spec.wayland_runtime_directory.join(WAYLAND_SOCKET),
            GUEST_WAYLAND_RUNTIME_DIRECTORY,
            WAYLAND_SOCKET,
            "Wayland",
        )?;
    }
    if virgl {
        bind_socket(
            &mut argv,
            &spec.rootfs,
            &spec.virgl_runtime_directory.join(VIRGL_SOCKET),
            GUEST_VIRGL_RUNTIME_DIRECTORY,
            VIRGL_SOCKET,
            "VirGL",
        )?;
    }
    if spec.launch_argv.is_empty() {
        if x11 {
            argv.extend([
                "/usr/bin/env".into(),
                "-u".into(),
                "WAYLAND_DISPLAY".into(),
                "DISPLAY=:0".into(),
                "XDG_SESSION_TYPE=x11".into(),
            ]);
        }
        argv.push(spec.shell.display().to_string());
        argv.push("-i".into());
    } else {
        if x11 {
            argv.extend([
                "/usr/bin/env".into(),
                "-u".into(),
                "WAYLAND_DISPLAY".into(),
                "DISPLAY=:0".into(),
                "XDG_SESSION_TYPE=x11".into(),
            ]);
        }
        argv.extend(spec.launch_argv.iter().cloned());
    }
    let mut env = vec![
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
    ];
    if x11 {
        env.push("DISPLAY=:0".into());
        env.push("XDG_SESSION_TYPE=x11".into());
    }
    if wayland {
        env.push(format!("XDG_RUNTIME_DIR={GUEST_WAYLAND_RUNTIME_DIRECTORY}"));
        env.push(format!("WAYLAND_DISPLAY={WAYLAND_SOCKET}"));
        env.push("XDG_SESSION_TYPE=wayland".into());
    }
    if virgl {
        let socket = format!("{GUEST_VIRGL_RUNTIME_DIRECTORY}/{VIRGL_SOCKET}");
        env.push(format!("VTEST_SOCKET_NAME={socket}"));
        env.push(format!("VTEST_RENDERER_SOCKET_NAME={socket}"));
    }
    validate_environment(&spec.graphics_environment)?;
    env.extend(spec.graphics_environment.iter().cloned());
    if wayland && udev_compatibility {
        env.push(format!("KDEWM={GUEST_KWIN_WRAPPER}"));
    }
    Ok((strings(&argv)?, strings(&env)?))
}

fn prepare_udev_compatibility_library(spec: &ProotSpec) -> Result<bool> {
    if spec.udev_compatibility_library.as_os_str().is_empty() {
        return Ok(false);
    }
    anyhow::ensure!(
        spec.udev_compatibility_library.is_file(),
        "guest compatibility library is missing: {}",
        spec.udev_compatibility_library.display(),
    );
    let destination = spec.rootfs.join(GUEST_UDEV_COMPATIBILITY_LIBRARY.trim_start_matches('/'));
    let parent = destination.parent().expect("compatibility library has a parent");
    std::fs::create_dir_all(parent)
        .with_context(|| format!("create guest compatibility directory: {}", parent.display()))?;
    let temporary = destination.with_extension("so.tmp");
    std::fs::copy(&spec.udev_compatibility_library, &temporary).with_context(|| {
        format!("copy guest compatibility library to {}", temporary.display())
    })?;
    std::fs::rename(&temporary, &destination).with_context(|| {
        format!("install guest compatibility library at {}", destination.display())
    })?;
    let wrapper = destination.with_file_name("kwin-wayland-wrapper");
    std::fs::write(&wrapper, kwin_wrapper_script(GUEST_UDEV_COMPATIBILITY_LIBRARY))
        .with_context(|| format!("write KWin compatibility wrapper: {}", wrapper.display()))?;
    std::fs::set_permissions(&wrapper, std::fs::Permissions::from_mode(0o755))
        .with_context(|| format!("mark KWin compatibility wrapper executable: {}", wrapper.display()))?;
    Ok(true)
}

fn kwin_wrapper_script(library: &str) -> String {
    format!(
        "#!/bin/sh\nif [ -x /usr/sbin/kwin_wayland_wrapper ]; then\n  exec /usr/bin/env LD_PRELOAD={library} /usr/sbin/kwin_wayland_wrapper \"$@\"\nfi\nexec /usr/bin/env LD_PRELOAD={library} /usr/bin/kwin_wayland \"$@\"\n"
    )
}

fn validate(spec: &ProotSpec, proot: &std::path::Path, loader: &std::path::Path) -> Result<()> {
    let guest_shell = spec.shell.to_string_lossy();
    anyhow::ensure!(
        spec.shell.is_absolute(),
        "shell must be an absolute path inside the rootfs"
    );
    if !spec.x11_socket_directory.as_os_str().is_empty() {
        anyhow::ensure!(
            spec.x11_socket_directory.is_absolute() && spec.x11_socket_directory.is_dir(),
            "X11 socket directory is not accessible: {}",
            spec.x11_socket_directory.display()
        );
        anyhow::ensure!(
            is_socket(&spec.x11_socket_directory.join("X0")),
            "Trierarch X11 socket is not ready"
        );
    }
    let wayland = !spec.wayland_runtime_directory.as_os_str().is_empty();
    anyhow::ensure!(
        !(wayland && !spec.x11_socket_directory.as_os_str().is_empty()),
        "X11 and Wayland cannot be selected for the same PRoot session"
    );
    if wayland {
        validate_socket(
            &spec.wayland_runtime_directory.join(WAYLAND_SOCKET),
            "Wayland runtime socket",
        )?;
    }
    if !spec.virgl_runtime_directory.as_os_str().is_empty() {
        validate_socket(
            &spec.virgl_runtime_directory.join(VIRGL_SOCKET),
            "VirGL runtime socket",
        )?;
    }
    for value in &spec.launch_argv {
        anyhow::ensure!(
            !value.is_empty() && !value.contains('\0'),
            "launch.argv must not be empty or contain a NUL byte"
        );
    }
    validate_environment(&spec.graphics_environment)?;
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
    anyhow::ensure!(
        spec.cache_dir.is_dir(),
        "PRoot cache directory does not exist"
    );
    Ok(())
}

fn validate_environment(values: &[String]) -> Result<()> {
    for value in values {
        let Some((name, _)) = value.split_once('=') else {
            anyhow::bail!("graphics environment entry must be NAME=VALUE");
        };
        anyhow::ensure!(
            !name.is_empty()
                && name
                    .bytes()
                    .all(|byte| byte == b'_' || byte.is_ascii_uppercase())
                && !value.contains('\0'),
            "graphics environment entry is invalid"
        );
    }
    Ok(())
}

fn is_socket(path: &std::path::Path) -> bool {
    std::fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_socket())
}

fn validate_socket(path: &std::path::Path, label: &str) -> Result<()> {
    anyhow::ensure!(
        path.is_absolute() && is_socket(path),
        "{label} is not accessible: {}",
        path.display()
    );
    Ok(())
}

fn bind_socket(
    argv: &mut Vec<String>,
    rootfs: &std::path::Path,
    host_socket: &std::path::Path,
    guest_directory: &str,
    socket_name: &str,
    label: &str,
) -> Result<()> {
    validate_socket(host_socket, &format!("{label} socket"))?;
    let guest_directory = rootfs.join(guest_directory.trim_start_matches('/'));
    std::fs::create_dir_all(&guest_directory).with_context(|| {
        format!(
            "create guest {label} directory: {}",
            guest_directory.display()
        )
    })?;
    let guest_socket = guest_directory.join(socket_name);
    if !guest_socket.exists() {
        std::fs::File::create(&guest_socket).with_context(|| {
            format!(
                "create guest {label} socket mountpoint: {}",
                guest_socket.display()
            )
        })?;
    }
    argv.push(format!(
        "--bind={}:/{}",
        host_socket.display(),
        guest_socket
            .strip_prefix(rootfs)
            .expect("guest socket is below rootfs")
            .display()
    ));
    Ok(())
}

fn strings(values: &[String]) -> Result<Vec<CString>> {
    values
        .iter()
        .map(|value| CString::new(value.as_bytes()).context("PRoot argv/environment contains NUL"))
        .collect()
}
