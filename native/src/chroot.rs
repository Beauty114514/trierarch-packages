//! Launch an already deployed chroot through the device's existing `su` provider.
//!
//! This module deliberately does not mount filesystems, install a rootfs, or
//! manage a persistent service. It only turns an existing rootfs plus guest
//! shell into one interactive PTY process.

use crate::LaunchSpec;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Debug)]
pub struct ChrootSpec {
    pub rootfs: PathBuf,
    pub shell: PathBuf,
}

impl ChrootSpec {
    pub(crate) fn launch_spec(&self) -> io::Result<LaunchSpec> {
        validate_guest_path(&self.rootfs, "rootfs")?;
        validate_guest_path(&self.shell, "shell")?;
        let su = find_su().ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "no supported su executable was found")
        })?;

        // `su -c` accepts one shell command. Quote every configuration-derived
        // token independently; no user input is interpreted as shell syntax.
        //
        // The environment is deliberately established *inside* the privileged
        // command. Some `su` implementations sanitise their inherited
        // environment, and Android's PATH would otherwise leak into the guest.
        let command = format!(
            "export HOME=/root TERM=xterm-256color LANG=C.UTF-8 USER=root \\
             LOGNAME=root TMP=/tmp TMPDIR=/tmp MAIL=/var/mail/root \\
             PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; \\
             exec /system/bin/chroot {} {} -i",
            shell_quote(&self.rootfs),
            shell_quote(&self.shell),
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
