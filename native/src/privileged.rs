//! Shared launch helpers for runtimes that enter through Android's root provider.

use std::ffi::OsStr;
use std::io;
use std::path::PathBuf;
use std::process::{Command, Output};

pub fn find_su() -> Option<PathBuf> {
    ["/system/bin/su", "/system/xbin/su", "/product/bin/su"]
        .into_iter()
        .map(PathBuf::from)
        .find(|path| path.is_file())
}

pub fn su_arguments(su: &PathBuf, command: String) -> Vec<String> {
    match identify_su(su) {
        SuFlavor::Magisk if su_version_code(su) == Some(28_100) => {
            vec!["-M".into(), "-c".into(), command]
        }
        // DroidSpaces' Android backend lives in the root mount namespace.
        SuFlavor::Magisk => vec!["-i".into(), "-M".into(), "-c".into(), command],
        SuFlavor::KernelSu => vec!["-M".into(), "-p".into(), "-c".into(), command],
        SuFlavor::Other => vec!["-c".into(), command],
    }
}

/// Run one deliberately constructed maintenance command through the device's
/// configured root provider.  Callers must quote every dynamic value.
pub fn run_as_root(command: String) -> io::Result<()> {
    let output = run_as_root_output(command)?;
    if output.status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "su exited with {}",
            output.status
        )))
    }
}

pub fn run_as_root_output(command: String) -> io::Result<Output> {
    let su = find_su().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "no supported su executable was found",
        )
    })?;
    Command::new(&su).args(su_arguments(&su, command)).output()
}

pub fn shell_quote(value: impl AsRef<OsStr>) -> String {
    let value = String::from_utf8_lossy(value.as_ref().as_encoded_bytes());
    format!("'{}'", value.replace('\'', "'\"'\"'"))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SuFlavor {
    Magisk,
    KernelSu,
    Other,
}

fn identify_su(su: &PathBuf) -> SuFlavor {
    let Ok(output) = Command::new(su).arg("--help").output() else {
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

fn su_version_code(su: &PathBuf) -> Option<u32> {
    let output = Command::new(su).arg("-V").output().ok()?;
    String::from_utf8(output.stdout).ok()?.trim().parse().ok()
}
