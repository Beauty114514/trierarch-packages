//! Start and attach Trierarch's PTY to a DroidSpaces container.

use crate::{privileged, LaunchSpec};
use std::io;
use std::path::{Path, PathBuf};

const DROIDSPACES_BINARY: &str = "/data/local/Droidspaces/bin/droidspaces";
const GUEST_X11_SOCKET: &str = "/tmp/.X11-unix/X0";
const GUEST_WAYLAND_HOST_DIRECTORY: &str = "/tmp/trierarch-wayland-host";
const GUEST_WAYLAND_RUNTIME_DIRECTORY: &str = "/tmp/trierarch-wayland-user";
const WAYLAND_SOCKET: &str = "wayland-trierarch";
const GUEST_VIRGL_RUNTIME_DIRECTORY: &str = "/tmp/trierarch-virgl-host";
const VIRGL_SOCKET: &str = "vtest.sock";
const GUEST_COMPATIBILITY_SOURCE_DIRECTORY: &str = "/tmp/trierarch-compat-source";
const GUEST_COMPATIBILITY_DIRECTORY: &str = "/tmp/trierarch-compat";
const GUEST_COMPATIBILITY_LIBRARY: &str = "/tmp/trierarch-compat/libtrierarch-udev-compat.so";
const GUEST_KWIN_WRAPPER: &str = "/tmp/trierarch-compat/kwin-wayland-wrapper";

#[derive(Clone, Debug)]
pub struct DroidspacesSpec {
    pub container: String,
    pub user: String,
    /// Empty means this is an ordinary terminal session.
    ///
    /// A non-empty value is attached only while starting a stopped container:
    /// DroidSpaces applies bind mounts during `start`, not `run`.
    pub x11_socket_directory: String,
    /// Host directory containing Trierarch's Wayland socket, if enabled.
    pub wayland_runtime_directory: String,
    /// Host directory containing Trierarch's VirGL vtest socket, if enabled.
    pub virgl_runtime_directory: String,
    /// App-private guest-glibc library, bind-mounted only while starting.
    pub udev_compatibility_library: String,
    /// Empty keeps DroidSpaces' interactive-shell default.
    pub launch_argv: Vec<String>,
    /// Rendering environment resolved from the profile by the Android app.
    pub graphics_environment: Vec<String>,
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
        validate_environment(&self.graphics_environment)?;
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
        let wayland_bind = self.wayland_bind()?;
        let virgl_bind = self.virgl_bind()?;
        let compatibility_bind = self.compatibility_bind()?;
        let start = self.start_command(
            x11_bind.as_deref(),
            wayland_bind.as_deref(),
            virgl_bind.as_deref(),
            compatibility_bind.as_deref(),
        );
        let run = if !self.wayland_runtime_directory.is_empty() {
            self.wayland_command()
        } else if self.x11_socket_directory.is_empty() {
            self.terminal_command()
        } else {
            self.x11_command()
        };
        // Android app-private bind mounts retain their app-UID permissions.
        // Copy the compatibility library while DroidSpaces is root, before a
        // graphical command switches to the configured guest user.
        let prepare_compatibility = self.compatibility_command();
        let run = format!("{prepare_compatibility}{run}");

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
        } else if wayland_bind.is_some() || virgl_bind.is_some() {
            Ok(format!(
                "export TERM=xterm-256color LANG=C.UTF-8; {running} {start} || exit $?;; *) printf '%s\\n' 'DroidSpaces container is already running without this Trierarch graphical attachment; stop it, then start this profile.' >&2; exit 125;; esac; {run}",
            ))
        } else {
            Ok(format!(
                "export TERM=xterm-256color LANG=C.UTF-8; {running} {start} || exit $?;; esac; {run}",
            ))
        }
    }

    fn start_command(
        &self,
        x11_bind: Option<&str>,
        wayland_bind: Option<&str>,
        virgl_bind: Option<&str>,
        compatibility_bind: Option<&str>,
    ) -> String {
        let mut bind_arguments = String::new();
        if let Some(bind) = x11_bind {
            bind_arguments.push_str(&format!(" --bind={}", privileged::shell_quote(bind)));
        }
        if let Some(bind) = wayland_bind {
            bind_arguments.push_str(&format!(" --bind={}", privileged::shell_quote(bind)));
        }
        if let Some(bind) = virgl_bind {
            bind_arguments.push_str(&format!(" --bind={}", privileged::shell_quote(bind)));
        }
        if let Some(bind) = compatibility_bind {
            bind_arguments.push_str(&format!(" --bind={}", privileged::shell_quote(bind)));
        }
        format!(
            "{} --name={}{} start",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            bind_arguments,
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
            let environment = shell_words(&self.guest_graphics_environment());
            format!(
                "export TERM=xterm-256color LANG=C.UTF-8; exec {} --name={} --user={} run /bin/sh -lc {}",
                privileged::shell_quote(DROIDSPACES_BINARY),
                privileged::shell_quote(&self.container),
                privileged::shell_quote(&self.user),
                privileged::shell_quote(&format!("exec /usr/bin/env {environment} {}", shell_words(&self.launch_argv))),
            )
        }
    }

    fn x11_command(&self) -> String {
        let graphics_environment = shell_words(&self.guest_graphics_environment());
        // DroidSpaces creates this per-user runtime directory as part of its
        // systemd container session.  Plasma/KWin uses it even in an X11
        // session, so preserve it across the login `su` below rather than
        // leaving the desktop with only DISPLAY and D-Bus.
        let xdg_runtime_directory = format!("/tmp/runtime-{}", self.user);
        let prefix = format!(
            "/usr/bin/env -u WAYLAND_DISPLAY -u QT_QPA_PLATFORM -u QT_QUICK_BACKEND DISPLAY=:0 XDG_SESSION_TYPE=x11 XDG_RUNTIME_DIR={} {graphics_environment}",
            privileged::shell_quote(&xdg_runtime_directory),
        );

        let guest = if self.launch_argv.is_empty() {
            // `enter` cannot receive the DISPLAY environment. Let guest `su`
            // select its own configured login shell while preserving only the
            // terminal and X11 variables required by this session.
            format!(
                "{prefix} /usr/bin/su -l -w DISPLAY,XDG_SESSION_TYPE,XDG_RUNTIME_DIR,QT_QUICK_BACKEND,LIBGL_ALWAYS_SOFTWARE,GALLIUM_DRIVER,MESA_LOADER_DRIVER_OVERRIDE,VTEST_SOCKET_NAME,VTEST_RENDERER_SOCKET_NAME,LD_PRELOAD,TERM {}",
                privileged::shell_quote(&self.user),
            )
        } else {
            format!(
                "{prefix} /usr/bin/su -l -w DISPLAY,XDG_SESSION_TYPE,XDG_RUNTIME_DIR,QT_QUICK_BACKEND,LIBGL_ALWAYS_SOFTWARE,GALLIUM_DRIVER,MESA_LOADER_DRIVER_OVERRIDE,VTEST_SOCKET_NAME,VTEST_RENDERER_SOCKET_NAME,LD_PRELOAD,TERM {} -c {}",
                privileged::shell_quote(&self.user),
                privileged::shell_quote(shell_words(&self.launch_argv)),
            )
        };
        format!(
            "exec {} --name={} run /bin/sh -lc {}",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            privileged::shell_quote(&format!("exec {guest}")),
        )
    }

    fn wayland_command(&self) -> String {
        let graphics_environment = shell_words(&self.wayland_graphics_environment());
        let prefix = format!(
            "exec {} --name={} --user={} run /usr/bin/env -u DISPLAY -u QT_QPA_PLATFORM -u QT_QUICK_BACKEND XDG_RUNTIME_DIR={} WAYLAND_DISPLAY={} XDG_SESSION_TYPE=wayland QT_QPA_PLATFORM=wayland {graphics_environment}",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            privileged::shell_quote(&self.user),
            GUEST_WAYLAND_RUNTIME_DIRECTORY,
            WAYLAND_SOCKET,
        );
        let prepare_runtime = format!(
            "install -d -m 700 {runtime} && ln -sfn {host}/{socket} {runtime}/{socket};",
            runtime = GUEST_WAYLAND_RUNTIME_DIRECTORY,
            host = GUEST_WAYLAND_HOST_DIRECTORY,
            socket = WAYLAND_SOCKET,
        );
        if self.launch_argv.is_empty() {
            format!("{prefix} /bin/sh -lc {}", privileged::shell_quote(&format!(
                "{} exec /bin/sh -l",
                prepare_runtime,
            )))
        } else {
            format!("{prefix} /bin/sh -lc {}", privileged::shell_quote(&format!(
                "{} exec {}",
                prepare_runtime,
                shell_words(&self.launch_argv),
            )))
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

    fn wayland_bind(&self) -> io::Result<Option<String>> {
        if self.wayland_runtime_directory.is_empty() { return Ok(None); }
        let directory = Path::new(&self.wayland_runtime_directory);
        if !directory.is_absolute() || !directory.is_dir() {
            return Err(io::Error::new(io::ErrorKind::NotFound,
                format!("Wayland runtime directory is not accessible: {}", directory.display())));
        }
        Ok(Some(format!(
            "{}:{GUEST_WAYLAND_HOST_DIRECTORY}",
            directory.display()
        )))
    }

    fn virgl_bind(&self) -> io::Result<Option<String>> {
        if self.virgl_runtime_directory.is_empty() { return Ok(None); }
        let directory = Path::new(&self.virgl_runtime_directory);
        if !directory.is_absolute() || !directory.is_dir() || !is_socket(&directory.join(VIRGL_SOCKET)) {
            return Err(io::Error::new(io::ErrorKind::NotFound,
                format!("VirGL runtime socket is not accessible: {}", directory.join(VIRGL_SOCKET).display())));
        }
        Ok(Some(format!("{}:{GUEST_VIRGL_RUNTIME_DIRECTORY}", directory.display())))
    }

    fn compatibility_bind(&self) -> io::Result<Option<String>> {
        if self.udev_compatibility_library.is_empty() {
            return Ok(None);
        }
        let library = Path::new(&self.udev_compatibility_library);
        if !library.is_absolute() || !library.is_file() {
            return Err(io::Error::new(io::ErrorKind::NotFound,
                format!("guest compatibility library is not accessible: {}", library.display())));
        }
        let source = library.parent().expect("compatibility library has a parent");
        Ok(Some(format!("{}:{GUEST_COMPATIBILITY_SOURCE_DIRECTORY}", source.display())))
    }

    fn guest_graphics_environment(&self) -> Vec<String> {
        let mut environment = self.graphics_environment.clone();
        if !self.virgl_runtime_directory.is_empty() {
            let socket = format!("{GUEST_VIRGL_RUNTIME_DIRECTORY}/{VIRGL_SOCKET}");
            environment.push(format!("VTEST_SOCKET_NAME={socket}"));
            environment.push(format!("VTEST_RENDERER_SOCKET_NAME={socket}"));
        }
        environment
    }

    /// `LD_PRELOAD` is deliberately not placed in the Plasma session
    /// environment: it would be inherited by D-Bus, plasmashell and every
    /// desktop application. Plasma's `KDEWM` hook instead starts this wrapper
    /// only for KWin, which is the sole consumer of the udev workaround.
    fn wayland_graphics_environment(&self) -> Vec<String> {
        let mut environment = self.guest_graphics_environment();
        if !self.udev_compatibility_library.is_empty() {
            environment.push(format!("KDEWM={GUEST_KWIN_WRAPPER}"));
        }
        environment
    }

    fn compatibility_prelude(&self) -> String {
        if self.udev_compatibility_library.is_empty() {
            return String::new();
        }
        let wrapper = format!(
            "#!/bin/sh\nif [ -x /usr/sbin/kwin_wayland_wrapper ]; then\n  exec /usr/bin/env LD_PRELOAD={library} /usr/sbin/kwin_wayland_wrapper \"$@\"\nfi\nexec /usr/bin/env LD_PRELOAD={library} /usr/bin/kwin_wayland \"$@\"\n",
            library = GUEST_COMPATIBILITY_LIBRARY,
        );
        format!(
            "if [ -f {source}/libtrierarch-udev-compat.so ]; then install -d -m 755 {directory} && install -m 755 {source}/libtrierarch-udev-compat.so {library} && printf '%s' {wrapper} > {kwin_wrapper} && chmod 755 {kwin_wrapper}; elif [ ! -f {library} ]; then printf '%s\\n' 'Trierarch guest compatibility library is unavailable; stop the container and start this profile again.' >&2; exit 126; fi;",
            directory = GUEST_COMPATIBILITY_DIRECTORY,
            source = GUEST_COMPATIBILITY_SOURCE_DIRECTORY,
            library = GUEST_COMPATIBILITY_LIBRARY,
            wrapper = privileged::shell_quote(&wrapper),
            kwin_wrapper = GUEST_KWIN_WRAPPER,
        )
    }

    fn compatibility_command(&self) -> String {
        if self.udev_compatibility_library.is_empty() {
            return String::new();
        }
        format!(
            "{} --name={} run /bin/sh -lc {} || exit $?; ",
            privileged::shell_quote(DROIDSPACES_BINARY),
            privileged::shell_quote(&self.container),
            privileged::shell_quote(&self.compatibility_prelude()),
        )
    }
}

fn is_socket(path: &Path) -> bool {
    use std::os::unix::fs::FileTypeExt;
    path.symlink_metadata()
        .map(|metadata| metadata.file_type().is_socket())
        .unwrap_or(false)
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

fn validate_value(value: &str, name: &str) -> io::Result<()> {
    if value.is_empty() || value.contains('\0') {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name} must not be empty or contain a NUL byte"),
        ));
    }
    Ok(())
}
