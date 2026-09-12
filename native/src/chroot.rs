//! Launch an already deployed chroot through the device's existing `su` provider.
//!
//! This module does not install a rootfs or manage a persistent service. It
//! prepares the standard kernel filesystems and display runtime mounts for
//! each session, then removes only the mounts it created.

use crate::LaunchSpec;
use std::io;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::process::Command;

const GUEST_WAYLAND_HOST_DIRECTORY: &str = "/tmp/trierarch-wayland-host";
const GUEST_WAYLAND_IME_BRIDGE: &str = "/opt/trierarch/wayland-ime/trierarch-wayland-ime-bridge";
const WAYLAND_SOCKET: &str = "wayland-trierarch";
const GUEST_UDEV_COMPATIBILITY_LIBRARY: &str = "/opt/trierarch/compat/libtrierarch-udev-compat.so";
const GUEST_KWIN_WAYLAND_WRAPPER: &str = "/usr/sbin/kwin_wayland_wrapper";
const GUEST_KWIN_WAYLAND_WRAPPER_PATHS: &[&str] = &[
    "/usr/bin/kwin_wayland_wrapper",
    "/usr/sbin/kwin_wayland_wrapper",
];
const GUEST_KWIN_WAYLAND_WRAPPER_REAL: &str = "/opt/trierarch/compat/kwin_wayland_wrapper.real";
const GUEST_KWIN_WAYLAND_WRAPPER_SHIM: &str = "/opt/trierarch/compat/kwin-wayland-wrapper";

#[derive(Clone, Debug)]
pub struct ChrootSpec {
    pub rootfs: PathBuf,
    pub shell: PathBuf,
    /// Empty means terminal-only; otherwise bind this host directory's X11 tmp.
    pub x11_socket_directory: PathBuf,
    /// Host directory containing Trierarch's Wayland runtime, if enabled.
    pub wayland_runtime_directory: PathBuf,
    /// Optional app-provided guest Wayland IME bridge executable.
    pub wayland_ime_bridge: PathBuf,
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
        validate_optional_file(&self.wayland_ime_bridge, "Wayland IME bridge")?;
        if !self.wayland_ime_bridge.as_os_str().is_empty()
            && self.wayland_runtime_directory.as_os_str().is_empty()
        {
            return Err(io::Error::new(io::ErrorKind::InvalidInput,
                "Wayland IME bridge requires a Wayland runtime"));
        }
        let wayland = !self.wayland_runtime_directory.as_os_str().is_empty();
        if wayland && (!self.wayland_runtime_directory.is_absolute() || !self.wayland_runtime_directory.is_dir()) {
            return Err(io::Error::new(io::ErrorKind::NotFound,
                format!("Wayland runtime directory is not accessible: {}", self.wayland_runtime_directory.display())));
        }
        let guest_command = guest_command(self, x11, wayland);
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

fn guest_command(spec: &ChrootSpec, x11: bool, wayland: bool) -> String {
    let command = if spec.launch_argv.is_empty() {
        format!("{} -i", shell_quote(&spec.shell))
    } else {
        shell_words(&spec.launch_argv)
    };
    let environment = spec.graphics_environment.clone();
    let install_compatibility = if spec.udev_compatibility_library.as_os_str().is_empty() {
        String::new()
    } else {
        let destination = spec
            .rootfs
            .join(GUEST_UDEV_COMPATIBILITY_LIBRARY.trim_start_matches('/'));
        let parent = destination.parent().expect("compatibility library has a parent");
        format!(
            "test -f {source} || {{ printf '%s\\n' 'Trierarch guest compatibility library is missing.' >&2; exit 126; }}; /system/bin/toybox mkdir -p {parent} && /system/bin/toybox cp {source} {destination} && /system/bin/toybox chmod 755 {destination} || exit $?; ",
            source = shell_quote(&spec.udev_compatibility_library),
            parent = shell_quote(parent),
            destination = shell_quote(&destination),
        )
    };
    let graphics_environment = shell_words(&environment);
    let install_ime_bridge = if spec.wayland_ime_bridge.as_os_str().is_empty() {
        String::new()
    } else {
        let destination = spec.rootfs.join(GUEST_WAYLAND_IME_BRIDGE.trim_start_matches('/'));
        let parent = destination.parent().expect("IME bridge has a parent");
        format!(
            "test -f {source} || {{ printf '%s\\n' 'Trierarch Wayland IME bridge is missing.' >&2; exit 126; }}; /system/bin/toybox mkdir -p {parent} && /system/bin/toybox cp {source} {destination} && /system/bin/toybox chmod 755 {destination} || exit $?; ",
            source = shell_quote(&spec.wayland_ime_bridge),
            parent = shell_quote(parent),
            destination = shell_quote(&destination),
        )
    };
    let guest = if x11 {
        format!(
            "/usr/bin/env -u WAYLAND_DISPLAY -u QT_QUICK_BACKEND DISPLAY=:0 XDG_SESSION_TYPE=x11 \
             TMPDIR=/tmp XDG_RUNTIME_DIR=/tmp XKB_CONFIG_ROOT=/usr/share/X11/xkb {graphics_environment} {command}"
        )
    } else if wayland {
        format!(
            "/usr/bin/env -u DISPLAY -u QT_QUICK_BACKEND \
             XDG_RUNTIME_DIR={GUEST_WAYLAND_HOST_DIRECTORY} WAYLAND_DISPLAY={WAYLAND_SOCKET} \
             XDG_SESSION_TYPE=wayland QT_QPA_PLATFORM=wayland {graphics_environment} {command}"
        )
    } else {
        format!("/usr/bin/env -u QT_QUICK_BACKEND {graphics_environment} {command}")
    };
    if !x11 && !wayland {
        let system_mounts = prepare_system_mounts(&spec.rootfs);
        let system_cleanup = cleanup_system_mounts(&spec.rootfs);
        return format!(
            "trierarch_mount_proc=0; trierarch_mount_sys=0; trierarch_mount_dev=0; trierarch_mount_devpts=0; \\
             cleanup() {{ {system_cleanup} }}; \\
             trap cleanup 0; trap 'cleanup; exit 143' HUP INT TERM; \\
             {install_compatibility}{install_ime_bridge}{system_mounts} \\
             /system/bin/chroot {rootfs} {guest}; status=$?; cleanup; trap - 0; exit $status",
            rootfs = shell_quote(&spec.rootfs),
        );
    }

    if !x11 {
        let runtime_target = spec.rootfs.join(GUEST_WAYLAND_HOST_DIRECTORY.trim_start_matches('/'));
        let kwin_wrapper_target = guest_kwin_wrapper_target(&spec.rootfs);
        let kwin_wrapper_real = spec
            .rootfs
            .join(GUEST_KWIN_WAYLAND_WRAPPER_REAL.trim_start_matches('/'));
        let kwin_wrapper_shim = spec
            .rootfs
            .join(GUEST_KWIN_WAYLAND_WRAPPER_SHIM.trim_start_matches('/'));
        let kwin_wrapper_parent = kwin_wrapper_shim
            .parent()
            .expect("KWin compatibility wrapper has a parent");
        let install_kwin_wrapper = if let Some(target) = kwin_wrapper_target.as_ref() {
            if spec.udev_compatibility_library.as_os_str().is_empty() {
                String::new()
            } else {
                let wrapper_script = kwin_wayland_wrapper_script(GUEST_UDEV_COMPATIBILITY_LIBRARY);
                format!(
                    "if ! /system/bin/toybox mountpoint -q {target}; then /system/bin/toybox mkdir -p {parent} && /system/bin/toybox cp {target} {real} && /system/bin/toybox chmod 755 {real} && printf '%s' {script} > {shim} && /system/bin/toybox chmod 755 {shim} && /system/bin/toybox mount --bind {shim} {target} || exit $?; trierarch_mount_kwin_wrapper=1; fi; ",
                    target = shell_quote(target),
                    parent = shell_quote(kwin_wrapper_parent),
                    real = shell_quote(&kwin_wrapper_real),
                    script = shell_quote(Path::new(&wrapper_script)),
                    shim = shell_quote(&kwin_wrapper_shim),
                )
            }
        } else {
            String::new()
        };
        let kwin_wrapper_cleanup_target = kwin_wrapper_target
            .as_deref()
            .unwrap_or_else(|| Path::new(GUEST_KWIN_WAYLAND_WRAPPER));
        let system_mounts = prepare_system_mounts(&spec.rootfs);
        let system_cleanup = cleanup_system_mounts(&spec.rootfs);
        return format!(
            "trierarch_mount_proc=0; trierarch_mount_sys=0; trierarch_mount_dev=0; trierarch_mount_devpts=0; trierarch_mount_wayland=0; trierarch_mount_kwin_wrapper=0; \\
             cleanup() {{ \\
                 if [ \"$trierarch_mount_kwin_wrapper\" = 1 ]; then /system/bin/toybox umount -l {kwin_wrapper_target} >/dev/null 2>&1 || true; fi; \\
                 if [ \"$trierarch_mount_wayland\" = 1 ]; then /system/bin/toybox umount -l {runtime_target} >/dev/null 2>&1 || true; fi; \\
                 {system_cleanup} \\
             }}; \\
             trap cleanup 0; trap 'cleanup; exit 143' HUP INT TERM; \\
             {system_mounts} \\
             {install_compatibility}{install_ime_bridge}{install_kwin_wrapper} \\
              mkdir -p {runtime_target} || exit $?; \\
              if ! /system/bin/toybox mountpoint -q {runtime_target}; then /system/bin/toybox mount --bind {source} {runtime_target} || exit $?; trierarch_mount_wayland=1; fi; \\
             /system/bin/chroot {rootfs} {guest}; status=$?; cleanup; trap - 0; exit $status",
            source = shell_quote(&spec.wayland_runtime_directory),
            runtime_target = shell_quote(&runtime_target),
            kwin_wrapper_target = shell_quote(kwin_wrapper_cleanup_target),
            rootfs = shell_quote(&spec.rootfs),
            install_compatibility = install_compatibility,
            install_ime_bridge = install_ime_bridge,
            install_kwin_wrapper = install_kwin_wrapper,
            system_mounts = system_mounts,
            system_cleanup = system_cleanup,
        );
    }

    let source = spec.x11_socket_directory.join("X0");
    let source_tmp = spec
        .x11_socket_directory
        .parent()
        .expect("X11 socket directory has a tmp parent");
    let target = spec.rootfs.join("tmp");
    let system_mounts = prepare_system_mounts(&spec.rootfs);
    let system_cleanup = cleanup_system_mounts(&spec.rootfs);
    format!(
        "trierarch_mount_proc=0; trierarch_mount_sys=0; trierarch_mount_dev=0; trierarch_mount_devpts=0; trierarch_mount_x11=0; \\
         cleanup() {{ \\
             if [ \"$trierarch_mount_x11\" = 1 ]; then /system/bin/toybox umount -l {target} >/dev/null 2>&1 || true; fi; \\
             {system_cleanup} \\
         }}; \\
         trap cleanup 0; trap 'cleanup; exit 143' HUP INT TERM; \\
         test -S {source} || {{ printf '%s\\n' 'Trierarch X11 socket is not ready.' >&2; exit 124; }}; \\
         {install_compatibility}{install_ime_bridge}{system_mounts} \\
         mkdir -p {target} || exit $?; \\
         if ! /system/bin/toybox mountpoint -q {target}; then /system/bin/toybox mount --bind {source_tmp} {target} || exit $?; trierarch_mount_x11=1; fi; \\
         /system/bin/chroot {rootfs} {guest}; status=$?; cleanup; trap - 0; exit $status",
        source = shell_quote(&source),
        source_tmp = shell_quote(source_tmp),
        target = shell_quote(&target),
        rootfs = shell_quote(&spec.rootfs),
        install_compatibility = install_compatibility,
        install_ime_bridge = install_ime_bridge,
        system_mounts = system_mounts,
        system_cleanup = system_cleanup,
    )
}

fn kwin_wayland_wrapper_script(library: &str) -> String {
    format!(
        "#!/bin/sh\nexec /usr/bin/env LD_PRELOAD={library} {real} \"$@\"\n",
        real = GUEST_KWIN_WAYLAND_WRAPPER_REAL,
    )
}

/// Creates the minimum kernel-backed filesystem expected by ordinary guest
/// programs.  A rootfs managed by another Android app may only have these
/// points in that app's mount namespace, so Trierarch must prepare its own.
fn prepare_system_mounts(rootfs: &Path) -> String {
    let proc = rootfs.join("proc");
    let sys = rootfs.join("sys");
    let dev = rootfs.join("dev");
    let devpts = dev.join("pts");
    format!(
        "mkdir -p {proc} {sys} {dev} {devpts} || exit $?; \
         if ! /system/bin/toybox mountpoint -q {proc}; then /system/bin/toybox mount -t proc proc {proc} || exit $?; trierarch_mount_proc=1; fi; \
         if ! /system/bin/toybox mountpoint -q {sys}; then /system/bin/toybox mount --bind /sys {sys} || exit $?; trierarch_mount_sys=1; fi; \
         if ! /system/bin/toybox mountpoint -q {dev}; then /system/bin/toybox mount --bind /dev {dev} || exit $?; trierarch_mount_dev=1; fi; \
         if ! /system/bin/toybox mountpoint -q {devpts}; then /system/bin/toybox mount -t devpts devpts {devpts} || exit $?; trierarch_mount_devpts=1; fi; ",
        proc = shell_quote(&proc),
        sys = shell_quote(&sys),
        dev = shell_quote(&dev),
        devpts = shell_quote(&devpts),
    )
}

fn cleanup_system_mounts(rootfs: &Path) -> String {
    let proc = rootfs.join("proc");
    let sys = rootfs.join("sys");
    let dev = rootfs.join("dev");
    let devpts = dev.join("pts");
    format!(
        "if [ \"${{trierarch_mount_devpts}}\" = 1 ]; then /system/bin/toybox umount -l {devpts} >/dev/null 2>&1 || true; fi; \
         if [ \"${{trierarch_mount_dev}}\" = 1 ]; then /system/bin/toybox umount -l {dev} >/dev/null 2>&1 || true; fi; \
         if [ \"${{trierarch_mount_sys}}\" = 1 ]; then /system/bin/toybox umount -l {sys} >/dev/null 2>&1 || true; fi; \
         if [ \"${{trierarch_mount_proc}}\" = 1 ]; then /system/bin/toybox umount -l {proc} >/dev/null 2>&1 || true; fi; ",
        proc = shell_quote(&proc),
        sys = shell_quote(&sys),
        dev = shell_quote(&dev),
        devpts = shell_quote(&devpts),
    )
}

fn guest_kwin_wrapper_target(rootfs: &Path) -> Option<PathBuf> {
    GUEST_KWIN_WAYLAND_WRAPPER_PATHS
        .iter()
        .map(|guest_path| rootfs.join(guest_path.trim_start_matches('/')))
        .find(|path| path.is_file())
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

fn validate_optional_file(path: &Path, name: &str) -> io::Result<()> {
    if path.as_os_str().is_empty() {
        return Ok(());
    }
    if !path.is_absolute() || !path.is_file() {
        return Err(io::Error::new(io::ErrorKind::NotFound,
            format!("{name} is not accessible: {}", path.display())));
    }
    Ok(())
}

fn shell_quote(value: &Path) -> String {
    let value = value.as_os_str().as_encoded_bytes();
    let value = String::from_utf8_lossy(value);
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}
