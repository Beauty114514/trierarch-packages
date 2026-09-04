//! Launch an already deployed chroot through the device's existing `su` provider.
//!
//! This module does not install a rootfs or manage a persistent service. For
//! an X11 profile it creates one temporary bind mount for the X11 runtime tmp.

use crate::LaunchSpec;
use std::io;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Debug)]
pub struct ChrootSpec {
    pub rootfs: PathBuf,
    pub shell: PathBuf,
    /// Empty means terminal-only; otherwise bind this host directory's X11 tmp.
    pub x11_socket_directory: PathBuf,
    /// Empty starts the configured interactive shell.
    pub launch_argv: Vec<String>,
    /// Rendering environment resolved from the profile by the Android app.
    pub graphics_environment: Vec<String>,
    /// App-private source copied into the rootfs by the privileged launcher.
    pub udev_compatibility_library: PathBuf,
}

impl ChrootSpec {
    pub(crate) fn launch_spec(&self) -> io::Result<LaunchSpec> {
        validate_guest_path(&self.rootfs, "rootfs")?;
        validate_guest_path(&self.shell, "shell")?;
        let su = find_su().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "no supported su executable was found",
            )
        })?;

        // `su -c` accepts one shell command. Quote every configuration-derived
        // token independently; no user input is interpreted as shell syntax.
        //
        // The environment is deliberately established *inside* the privileged
        // command. Some `su` implementations sanitise their inherited
        // environment, and Android's PATH would otherwise leak into the guest.
        let x11 = !self.x11_socket_directory.as_os_str().is_empty();
        if x11 {
            validate_x11_socket(&self.x11_socket_directory)?;
        }
        validate_argv(&self.launch_argv)?;
        validate_environment(&self.graphics_environment)?;
        let guest_command = guest_command(self, x11);
        let command = format!(
            "export HOME=/root TERM=xterm-256color LANG=C.UTF-8 USER=root \\
             LOGNAME=root TMP=/tmp TMPDIR=/tmp MAIL=/var/mail/root \\
             PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; \\
             {guest_command}",
        );
        let arguments = su_arguments(&su, command);
        Ok(LaunchSpec {
            command: su,
            arguments,
            working_directory: PathBuf::from("/"),
            environment: vec![
                "TERM=xterm-256color".into(),
                // `su` is always executed by its absolute path. This host PATH
                // exists only for its own implementation, never for the guest.
                "PATH=/product/bin:/system/bin:/system/xbin".into(),
            ],
        })
    }
}

fn guest_command(spec: &ChrootSpec, x11: bool) -> String {
    let command = if spec.launch_argv.is_empty() {
        format!("{} -i", shell_quote(&spec.shell))
    } else {
        shell_words(&spec.launch_argv)
    };
    let mut environment = spec.graphics_environment.clone();
    let install_compatibility = if spec.udev_compatibility_library.as_os_str().is_empty() {
        String::new()
    } else {
        let destination = spec.rootfs.join("opt/trierarch/compat/libtrierarch-udev-compat.so");
        let parent = destination.parent().expect("compatibility library has a parent");
        let wrapper = parent.join("kwin-wayland-wrapper");
        let wrapper_script = kwin_wrapper_script("/opt/trierarch/compat/libtrierarch-udev-compat.so");
        // `LD_PRELOAD` must not be exported by the desktop-session parent.
        // Plasma reads KDEWM and starts this wrapper only for KWin.
        if !x11 {
            environment.push("KDEWM=/opt/trierarch/compat/kwin-wayland-wrapper".into());
        }
        format!(
            "test -f {source} || {{ printf '%s\\n' 'Trierarch guest compatibility library is missing.' >&2; exit 126; }}; /system/bin/toybox mkdir -p {parent} && /system/bin/toybox cp {source} {destination} && /system/bin/toybox chmod 755 {destination} && printf '%s' {wrapper_script} > {wrapper} && /system/bin/toybox chmod 755 {wrapper} || exit $?; ",
            source = shell_quote(&spec.udev_compatibility_library),
            parent = shell_quote(parent),
            destination = shell_quote(&destination),
            wrapper_script = shell_quote(Path::new(&wrapper_script)),
            wrapper = shell_quote(&wrapper),
        )
    };
    let graphics_environment = shell_words(&environment);
    let guest = if x11 {
        format!(
            "/usr/bin/env -u WAYLAND_DISPLAY -u QT_QUICK_BACKEND DISPLAY=:0 XDG_SESSION_TYPE=x11 \
             TMPDIR=/tmp XDG_RUNTIME_DIR=/tmp XKB_CONFIG_ROOT=/usr/share/X11/xkb {graphics_environment} {command}"
        )
    } else {
        format!("/usr/bin/env -u QT_QUICK_BACKEND {graphics_environment} {command}")
    };
    if !x11 {
        return format!(
            "{install_compatibility} exec /system/bin/chroot {} {guest}",
            shell_quote(&spec.rootfs),
        );
    }

    let source = spec.x11_socket_directory.join("X0");
    let source_tmp = spec
        .x11_socket_directory
        .parent()
        .expect("X11 socket directory has a tmp parent");
    let target = spec.rootfs.join("tmp");
    format!(
        "{install_compatibility} test -S {source} || {{ printf '%s\\n' 'Trierarch X11 socket is not ready.' >&2; exit 124; }}; \\
         mkdir -p {target}; \\
         /system/bin/toybox mount --bind {source_tmp} {target} || exit $?; \\
         cleanup() {{ /system/bin/toybox umount {target} >/dev/null 2>&1 || true; }}; \\
         trap 'cleanup; exit 143' HUP INT TERM; \\
         /system/bin/chroot {rootfs} {guest}; status=$?; cleanup; exit $status",
        source = shell_quote(&source),
        source_tmp = shell_quote(source_tmp),
        target = shell_quote(&target),
        rootfs = shell_quote(&spec.rootfs),
        install_compatibility = install_compatibility,
    )
}

fn kwin_wrapper_script(library: &str) -> String {
    format!(
        "#!/bin/sh\nif [ -x /usr/sbin/kwin_wayland_wrapper ]; then\n  exec /usr/bin/env LD_PRELOAD={library} /usr/sbin/kwin_wayland_wrapper \"$@\"\nfi\nexec /usr/bin/env LD_PRELOAD={library} /usr/bin/kwin_wayland \"$@\"\n"
    )
}

fn validate_environment(values: &[String]) -> io::Result<()> {
    for value in values {
        let Some((name, _)) = value.split_once('=') else {
            return Err(io::Error::new(io::ErrorKind::InvalidInput,
                "graphics environment entry must be NAME=VALUE"));
        };
        if name.is_empty()
            || !name.bytes().all(|byte| byte == b'_' || byte.is_ascii_uppercase())
            || value.contains('\0') {
            return Err(io::Error::new(io::ErrorKind::InvalidInput,
                "graphics environment entry is invalid"));
        }
    }
    Ok(())
}

fn validate_x11_socket(directory: &Path) -> io::Result<()> {
    if !directory.is_absolute() || !directory.is_dir() || !is_socket(&directory.join("X0")) {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "X11 socket directory is not accessible: {}",
                directory.display()
            ),
        ));
    }
    Ok(())
}

fn validate_argv(argv: &[String]) -> io::Result<()> {
    if argv
        .iter()
        .any(|value| value.is_empty() || value.contains('\0'))
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "launch.argv must not be empty or contain a NUL byte",
        ));
    }
    Ok(())
}

fn shell_words(values: &[String]) -> String {
    values
        .iter()
        .map(|value| shell_quote(Path::new(value)))
        .collect::<Vec<_>>()
        .join(" ")
}

fn is_socket(path: &Path) -> bool {
    std::fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_socket())
}

fn find_su() -> Option<PathBuf> {
    ["/system/bin/su", "/system/xbin/su", "/product/bin/su"]
        .into_iter()
        .map(PathBuf::from)
        .find(|path| path.is_file())
}

/// Select only the flags that belong to the installed `su` implementation.
///
/// Magisk's `-i` is essential for an interactive `-c` session: it allocates a
/// pseudo-terminal, preserving the terminal relationship that Bash needs for
/// job control. KernelSU uses a different command-line contract. Unknown
/// providers retain portable `su -c` behaviour instead of receiving flags they
/// may reject.
fn su_arguments(su: &Path, command: String) -> Vec<String> {
    match identify_su(su) {
        SuFlavor::Magisk if su_version_code(su) == Some(28_100) => {
            vec!["-M".into(), "-c".into(), command]
        }
        SuFlavor::Magisk => vec!["-i".into(), "-M".into(), "-c".into(), command],
        SuFlavor::KernelSu => vec!["-M".into(), "-p".into(), "-c".into(), command],
        SuFlavor::Other => vec!["-c".into(), command],
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SuFlavor {
    Magisk,
    KernelSu,
    Other,
}

fn identify_su(su: &Path) -> SuFlavor {
    let output = Command::new(su).arg("--help").output();
    let Ok(output) = output else {
        return SuFlavor::Other;
    };
    let help = String::from_utf8_lossy(&output.stdout);
    if help.contains("MagiskSU") {
        SuFlavor::Magisk
    } else if help.contains("KernelSU") {
        SuFlavor::KernelSu
    } else {
        SuFlavor::Other
    }
}

fn su_version_code(su: &Path) -> Option<u32> {
    let output = Command::new(su).arg("-V").output().ok()?;
    String::from_utf8(output.stdout).ok()?.trim().parse().ok()
}

fn validate_guest_path(path: &Path, name: &str) -> io::Result<()> {
    if !path.is_absolute() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name} must be an absolute path"),
        ));
    }
    if path.as_os_str().as_encoded_bytes().contains(&0) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name} must not contain a NUL byte"),
        ));
    }
    Ok(())
}

fn shell_quote(value: &Path) -> String {
    let value = value.as_os_str().as_encoded_bytes();
    let value = String::from_utf8_lossy(value);
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}
