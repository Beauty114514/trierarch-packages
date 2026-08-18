//! Start and attach Trierarch's PTY to a DroidSpaces container.

use crate::{privileged, LaunchSpec};
use std::io;
use std::path::{Path, PathBuf};

const DROIDSPACES_BINARY: &str = "/data/local/Droidspaces/bin/droidspaces";
const GUEST_X11_SOCKET: &str = "/tmp/.X11-unix/X0";

#[derive(Clone, Debug)]
pub struct DroidspacesSpec {
    pub container: String,
    pub user: String,
    /// Empty means this is an ordinary terminal session.
    ///
    /// A non-empty value is attached only while starting a stopped container:
    /// DroidSpaces applies bind mounts during `start`, not `run`.
    pub x11_socket_directory: String,
    /// Empty keeps DroidSpaces' interactive-shell default.
    pub launch_argv: Vec<String>,
}

impl DroidspacesSpec {
    pub(crate) fn is_container_running(container: &str) -> io::Result<bool> {
        validate_value(container, "container")?;
        let output = privileged::run_as_root_output(format!(
            "{} --name={} pid",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(container),
        ))?;
        if !output.status.success() {
            return Err(io::Error::other(format!(
                "DroidSpaces pid exited with {}",
                output.status
            )));
        }
        Ok(String::from_utf8_lossy(&output.stdout)
            .trim()
            .parse::<u32>()
            .is_ok_and(|pid| pid > 0))
    }

    pub(crate) fn stop_container(container: &str) -> io::Result<()> {
        validate_value(container, "container")?;
        privileged::run_as_root(format!(
            "exec {} --name={} stop",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(container),
        ))
    }

    pub(crate) fn launch_spec(&self) -> io::Result<LaunchSpec> {
        validate_value(&self.container, "container")?;
        validate_value(&self.user, "user")?;
        validate_argv(&self.launch_argv)?;
        let su = privileged::find_su().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                "no supported su executable was found",
            )
        })?;

        let command = self.session_command()?;
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

    fn session_command(&self) -> io::Result<String> {
        let x11_bind = self.x11_bind()?;
        let start = self.start_command(x11_bind.as_deref());
        let run = if self.x11_socket_directory.is_empty() {
            self.terminal_command()
        } else {
            self.x11_command()
        };

        // `run` only joins an existing namespace. A bind option supplied to
        // it is ignored by DroidSpaces, so Trierarch owns the lifecycle:
        // start a stopped container with its requested attachments, then open
        // the requested terminal or graphical session.
        let running = format!(
            "container_pid=$({} --name={} pid 2>/dev/null || true); case \"$container_pid\" in ''|*[!0-9]*)",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
        );
        if let Some(x11_bind) = x11_bind {
            let host_socket = x11_bind
                .split_once(':')
                .map(|(source, _)| source)
                .expect("X11 bind always has a source and destination");
            let wait_for_socket = format!(
                "x11_wait=0; while [ ! -S {} ] && [ \"$x11_wait\" -lt 100 ]; do sleep 0.05; x11_wait=$((x11_wait + 1)); done; [ -S {} ] || {{ printf '%s\\n' 'Timed out waiting for the Trierarch X11 socket.' >&2; exit 124; }};",
                privileged::shell_quote(host_socket),
                privileged::shell_quote(host_socket),
            );
            Ok(format!(
                "export TERM=xterm-256color LANG=C.UTF-8; {wait_for_socket} {running} {start} || exit $?;; *) printf '%s\\n' 'DroidSpaces container is already running without this X11 attachment; stop it, then start this Trierarch profile.' >&2; exit 125;; esac; {run}",
            ))
        } else {
            Ok(format!(
                "export TERM=xterm-256color LANG=C.UTF-8; {running} {start} || exit $?;; esac; {run}",
            ))
        }
    }

    fn start_command(&self, x11_bind: Option<&str>) -> String {
        let bind_argument = x11_bind.map_or_else(String::new, |bind| {
            format!(" --bind={}", privileged::shell_quote(bind))
        });
        format!(
            "{} --name={}{} start",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            bind_argument,
        )
    }

    fn terminal_command(&self) -> String {
        if self.launch_argv.is_empty() {
            format!(
                "export TERM=xterm-256color LANG=C.UTF-8; exec {} --name={} enter {}",
                privileged::shell_quote(DROIDSPACES_BINARY),
                privileged::shell_quote(&self.container),
                privileged::shell_quote(&self.user),
            )
        } else {
            format!(
                "export TERM=xterm-256color LANG=C.UTF-8; exec {} --name={} --user={} run {}",
                privileged::shell_quote(DROIDSPACES_BINARY),
                privileged::shell_quote(&self.container),
                privileged::shell_quote(&self.user),
                shell_words(&self.launch_argv),
            )
        }
    }

    fn x11_command(&self) -> String {
        let prefix = format!(
            "exec {} --name={} run /usr/bin/env -u WAYLAND_DISPLAY DISPLAY=:0 XDG_SESSION_TYPE=x11",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
        );

        if self.launch_argv.is_empty() {
            // `enter` cannot receive the DISPLAY environment. Let guest `su`
            // select its own configured login shell while preserving only the
            // terminal and X11 variables required by this session.
            format!(
                "{prefix} /usr/bin/su -l -w DISPLAY,XDG_SESSION_TYPE,TERM {}",
                privileged::shell_quote(&self.user),
            )
        } else {
            format!(
                "{prefix} /usr/bin/su -l -w DISPLAY,XDG_SESSION_TYPE,TERM {} -c {}",
                privileged::shell_quote(&self.user),
                privileged::shell_quote(shell_words(&self.launch_argv)),
            )
        }
    }

    fn x11_bind(&self) -> io::Result<Option<String>> {
        if self.x11_socket_directory.is_empty() {
            return Ok(None);
        }
        let socket_directory = Path::new(&self.x11_socket_directory);
        if !socket_directory.is_absolute() || !socket_directory.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "X11 socket directory is not accessible: {}",
                    socket_directory.display()
                ),
            ));
        }
        Ok(Some(format!(
            "{}/X0:{GUEST_X11_SOCKET}",
            socket_directory.display()
        )))
    }
}

fn shell_words(values: &[String]) -> String {
    values
        .iter()
        .map(privileged::shell_quote)
        .collect::<Vec<_>>()
        .join(" ")
}

fn validate_argv(argv: &[String]) -> io::Result<()> {
    for value in argv {
        validate_value(value, "launch.argv")?;
    }
    Ok(())
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
