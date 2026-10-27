//! Android-side PipeWire playback host for DroidSpaces.
//!
//! The package payload supplies PipeWire, its PulseAudio-protocol server, and
//! the Android AAudio sink module. The service sockets are created before
//! DroidSpaces starts so they can be bind-mounted into its namespace.

use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

const SOCKET_NAME: &str = "pipewire-0";
const PULSE_SOCKET: &str = "pulse/native";
const COMPONENTS: [&str; 2] = ["pipewire", "pipewire-pulse"];

struct Host {
    runtime_directory: PathBuf,
    children: Vec<Child>,
}

static HOST: OnceLock<Mutex<Option<Host>>> = OnceLock::new();

fn host_slot() -> &'static Mutex<Option<Host>> {
    HOST.get_or_init(|| Mutex::new(None))
}

pub fn start(
    runtime_directory: &Path,
    payload_directory: &Path,
    native_library_directory: &Path,
) -> io::Result<()> {
    let mut slot = host_slot()
        .lock()
        .map_err(|_| io::Error::other("PipeWire host state lock poisoned"))?;
    if let Some(host) = slot.as_mut() {
        if host.runtime_directory == runtime_directory
            && host.is_running()?
            && sockets_ready(runtime_directory)
        {
            return Ok(());
        }
    }
    stop_host(&mut slot);

    let bin = payload_directory.join("bin");
    for component in COMPONENTS {
        let executable = bin.join(component);
        if !executable.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "PipeWire host component is missing: {}",
                    executable.display()
                ),
            ));
        }
    }
    for config in [
        "pipewire/pipewire.conf",
        "pipewire/pipewire-pulse.conf",
    ] {
        let path = payload_directory.join("etc").join(config);
        if !path.exists() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("PipeWire host configuration is missing: {}", path.display()),
            ));
        }
    }

    fs::create_dir_all(runtime_directory)?;
    fs::create_dir_all(runtime_directory.join("pulse"))?;
    // DroidSpaces clients use a different UID from the Android app. The
    // directory is searchable but not listable; only the known sockets are
    // made connectable across the bind mount.
    fs::set_permissions(runtime_directory, fs::Permissions::from_mode(0o711))?;
    fs::set_permissions(
        runtime_directory.join("pulse"),
        fs::Permissions::from_mode(0o711),
    )?;
    for socket in [
        socket_path(runtime_directory),
        pulse_socket_path(runtime_directory),
    ] {
        if socket.exists() {
            fs::remove_file(socket)?;
        }
    }

    let log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(runtime_directory.join("pipewire_host.log"))?;
    let linker = if cfg!(target_pointer_width = "64") {
        "/system/bin/linker64"
    } else {
        "/system/bin/linker"
    };
    let parent_pid = unsafe { libc::getpid() };
    let library_path = format!(
        "{}:{}:{}:{}:/system/lib64",
        payload_directory.join("lib").display(),
        payload_directory.join("lib/pipewire-0.3").display(),
        payload_directory.join("lib/spa-0.2").display(),
        native_library_directory.display(),
    );
    let mut children = Vec::new();
    for component in COMPONENTS {
        let executable = bin.join(component);
        let mut command = Command::new(linker);
        command
            .arg(&executable)
            .current_dir(runtime_directory)
            .env("HOME", runtime_directory)
            .env("XDG_RUNTIME_DIR", runtime_directory)
            .env("PIPEWIRE_RUNTIME_DIR", runtime_directory)
            .env("PULSE_RUNTIME_PATH", runtime_directory)
            .env(
                "PIPEWIRE_CONFIG_DIR",
                payload_directory.join("etc/pipewire"),
            )
            .env(
                "PIPEWIRE_MODULE_DIR",
                payload_directory.join("lib/pipewire-0.3"),
            )
            .env("SPA_PLUGIN_DIR", payload_directory.join("lib/spa-0.2"))
            .env("LD_LIBRARY_PATH", &library_path)
            .stdin(Stdio::null())
            .stdout(Stdio::from(log.try_clone()?))
            .stderr(Stdio::from(log.try_clone()?));
        match component {
            "pipewire" => {
                command
                    .arg("-c")
                    .arg(payload_directory.join("etc/pipewire/pipewire.conf"));
            }
            "pipewire-pulse" => {
                command
                    .arg("-c")
                    .arg(payload_directory.join("etc/pipewire/pipewire-pulse.conf"));
            }
            _ => unreachable!("only PipeWire playback daemons are started"),
        }
        // PipeWire creates its sockets with the process umask. Leave regular
        // files private while allowing a foreign container UID to connect.
        unsafe {
            command.pre_exec(move || {
                if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM) != 0 {
                    return Err(io::Error::last_os_error());
                }
                if libc::getppid() != parent_pid {
                    return Err(io::Error::new(
                        io::ErrorKind::Interrupted,
                        "Trierarch exited while PipeWire was starting",
                    ));
                }
                libc::umask(0o111);
                Ok(())
            });
        }
        match command.spawn() {
            Ok(child) => children.push(child),
            Err(error) => {
                stop_children(&mut children);
                return Err(error);
            }
        }
    }

    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        chmod_socket(&socket_path(runtime_directory));
        chmod_socket(&pulse_socket_path(runtime_directory));
        if sockets_ready(runtime_directory) {
            *slot = Some(Host {
                runtime_directory: runtime_directory.to_path_buf(),
                children,
            });
            return Ok(());
        }
        if let Some(status) = first_exit(&mut children)? {
            stop_children(&mut children);
            return Err(io::Error::other(format!(
                "PipeWire host exited before opening its client sockets: {status}"
            )));
        }
        thread::sleep(Duration::from_millis(25));
    }
    stop_children(&mut children);
    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "Timed out waiting for PipeWire and PulseAudio-protocol sockets",
    ))
}

pub fn stop() {
    if let Ok(mut slot) = host_slot().lock() {
        stop_host(&mut slot);
    }
}

impl Host {
    fn is_running(&mut self) -> io::Result<bool> {
        for child in &mut self.children {
            if child.try_wait()?.is_some() {
                return Ok(false);
            }
        }
        Ok(!self.children.is_empty())
    }
}

fn stop_host(slot: &mut Option<Host>) {
    if let Some(mut host) = slot.take() {
        stop_children(&mut host.children);
        let _ = fs::remove_file(socket_path(&host.runtime_directory));
        let _ = fs::remove_file(pulse_socket_path(&host.runtime_directory));
    }
}

fn stop_children(children: &mut Vec<Child>) {
    for child in children.iter_mut().rev() {
        let _ = child.kill();
    }
    for child in children.iter_mut().rev() {
        let _ = child.wait();
    }
    children.clear();
}

fn first_exit(children: &mut [Child]) -> io::Result<Option<std::process::ExitStatus>> {
    for child in children {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
    }
    Ok(None)
}

fn sockets_ready(runtime_directory: &Path) -> bool {
    is_socket(&socket_path(runtime_directory)) && is_socket(&pulse_socket_path(runtime_directory))
}

fn chmod_socket(path: &Path) {
    if is_socket(path) {
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o666));
    }
}

fn is_socket(path: &Path) -> bool {
    path.symlink_metadata()
        .map(|metadata| metadata.file_type().is_socket())
        .unwrap_or(false)
}

fn socket_path(runtime_directory: &Path) -> PathBuf {
    runtime_directory.join(SOCKET_NAME)
}

fn pulse_socket_path(runtime_directory: &Path) -> PathBuf {
    runtime_directory.join(PULSE_SOCKET)
}
