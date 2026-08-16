//! Attach Trierarch's PTY to an already-running DroidSpaces container.

use crate::{privileged, LaunchSpec};
use std::io;
use std::path::PathBuf;

const DROIDSPACES_BINARY: &str = "/data/local/Droidspaces/bin/droidspaces";

#[derive(Clone, Debug)]
pub struct DroidspacesSpec {
    pub container: String,
    pub user: String,
}

impl DroidspacesSpec {
    pub(crate) fn launch_spec(&self) -> io::Result<LaunchSpec> {
        validate_value(&self.container, "container")?;
        validate_value(&self.user, "user")?;
        let su = privileged::find_su().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "no supported su executable was found",
            )
        })?;
        // DroidSpaces remains the owner of rootfs, mounts, networking, and lifecycle.
        let command = format!(
            "export TERM=xterm-256color LANG=C.UTF-8; exec {} --name={} enter {}",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            privileged::shell_quote(&self.user),
        );
        Ok(LaunchSpec {
            command: su.clone(),
            arguments: privileged::su_arguments(&su, command),
            working_directory: PathBuf::from("/"),
            environment: vec![
                "TERM=xterm-256color".into(),
                "PATH=/product/bin:/system/bin:/system/xbin".into(),
            ],
        })
    }
}

fn validate_value(value: &str, name: &str) -> io::Result<()> {
    if value.is_empty() || value.contains('\0') {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name} must not be empty or contain a NUL byte"),
        ));
    }
    Ok(())
}
