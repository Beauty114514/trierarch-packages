//! Android-side PipeWire host using the standalone AAudio sink architecture.
//!
//! PipeWire and WirePlumber run in the Android app process context. A separate
//! PipeWire client owns the AAudio stream and registers an Audio/Sink node.
//! DroidSpaces receives the native and PulseAudio-compatible sockets through
//! the session's `/tmp/trierarch-pipewire` bind mount.

use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

const PIPEWIRE_SOCKET: &str = "pipewire-0";
const PULSE_SOCKET: &str = "pulse/native";
const PIPEWIRE_EXECUTABLES: [&str; 4] =
    ["pipewire", "wireplumber", "aaudio-sink", "pipewire-pulse"];

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

    let bin_directory = payload_directory.join("bin");
    for component in PIPEWIRE_EXECUTABLES {
        let executable = bin_directory.join(component);
        if !executable.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "PipeWire runtime component is missing: {}",
                    executable.display()
                ),
            ));
        }
    }
    let library_directory = payload_directory.join("lib");
    let spa_directory = payload_directory.join("lib/spa-0.2");
    for library in [
        "libpipewire-0.3.so",
        "libspa-support.so",
        "libspa-audioconvert.so",
    ] {
        let path = library_directory.join(library);
        if !path.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("PipeWire runtime library is missing: {}", path.display()),
            ));
        }
    }

    fs::create_dir_all(runtime_directory)?;
    fs::create_dir_all(runtime_directory.join("pulse"))?;
    let config_directory = runtime_directory.join("config");
    fs::create_dir_all(&config_directory)?;
    let share_directory = runtime_directory.join("share");
    fs::create_dir_all(&share_directory)?;
    // The WirePlumber share archive is a regular APK asset (Android asset
    // packaging does not preserve a generated nested directory tree).
    let share_archive = File::open(payload_directory.join("share/wireplumber-share.tar"))?;
    tar::Archive::new(share_archive).unpack(&share_directory)?;

    // DroidSpaces uses a different UID from the Android app. Keep directories
    // searchable and grant access only through the known service sockets.
    fs::set_permissions(runtime_directory, fs::Permissions::from_mode(0o711))?;
    fs::set_permissions(
        runtime_directory.join("pulse"),
        fs::Permissions::from_mode(0o711),
    )?;
    cleanup_sockets(runtime_directory);

    let pipewire_config = write_pipewire_config(&config_directory)?;
    let pulse_config = write_pipewire_pulse_config(&config_directory, runtime_directory)?;
    write_client_config(&config_directory)?;

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
        "{}:{}:{}:/system/lib64:/system/lib",
        library_directory.display(),
        native_library_directory.display(),
        payload_directory.join("lib/spa-0.2").display(),
    );
    let env = RuntimeEnv {
        runtime_directory,
        config_directory: &config_directory,
        library_directory: &library_directory,
        spa_directory: &spa_directory,
        share_directory: &share_directory,
        library_path: &library_path,
    };

    let mut children = Vec::new();
    let result = (|| {
        let pipewire = spawn_component(
            linker,
            &bin_directory.join("pipewire"),
            &["-c", pipewire_config.to_str().unwrap_or_default()],
            &env,
            parent_pid,
            &log,
        )?;
        children.push(pipewire);
        wait_for_socket(
            "pipewire",
            children.last_mut().unwrap(),
            &socket_path(runtime_directory),
        )?;

        children.push(spawn_component(
            linker,
            &bin_directory.join("wireplumber"),
            &[],
            &env,
            parent_pid,
            &log,
        )?);
        ensure_child_running("wireplumber", children.last_mut().unwrap())?;

        children.push(spawn_component(
            linker,
            &bin_directory.join("aaudio-sink"),
            &[
                "--node-name",
                "trierarch-aaudio-sink",
                "--rate",
                "48000",
                "--channels",
                "2",
                "--buffer-ms",
                "120",
            ],
            &env,
            parent_pid,
            &log,
        )?);
        ensure_child_running("AAudio sink", children.last_mut().unwrap())?;

        children.push(spawn_component(
            linker,
            &bin_directory.join("pipewire-pulse"),
            &["-c", pulse_config.to_str().unwrap_or_default()],
            &env,
            parent_pid,
            &log,
        )?);
        wait_for_socket(
            "pipewire-pulse",
            children.last_mut().unwrap(),
            &pulse_socket_path(runtime_directory),
        )?;
        Ok(())
    })();

    if let Err(error) = result {
        stop_children(&mut children);
        cleanup_sockets(runtime_directory);
        return Err(error);
    }

    // chmod after creation so clients with the guest UID can connect.
    chmod_socket(&socket_path(runtime_directory));
    chmod_socket(&pulse_socket_path(runtime_directory));
    *slot = Some(Host {
        runtime_directory: runtime_directory.to_path_buf(),
        children,
    });
    Ok(())
}

struct RuntimeEnv<'a> {
    runtime_directory: &'a Path,
    config_directory: &'a Path,
    library_directory: &'a Path,
    spa_directory: &'a Path,
    share_directory: &'a Path,
    library_path: &'a str,
}

fn spawn_component(
    linker: &str,
    executable: &Path,
    args: &[&str],
    env: &RuntimeEnv<'_>,
    parent_pid: libc::pid_t,
    log: &File,
) -> io::Result<Child> {
    let mut command = Command::new(linker);
    command
        .arg(executable)
        .args(args)
        .current_dir(env.runtime_directory)
        .env("HOME", env.runtime_directory)
        .env("XDG_RUNTIME_DIR", env.runtime_directory)
        .env("PIPEWIRE_RUNTIME_DIR", env.runtime_directory)
        .env("PULSE_RUNTIME_PATH", env.runtime_directory)
        .env("PIPEWIRE_CONFIG_DIR", env.config_directory)
        .env("PIPEWIRE_MODULE_DIR", env.library_directory)
        .env("SPA_PLUGIN_DIR", env.spa_directory)
        .env(
            "WIREPLUMBER_CONFIG_DIR",
            env.share_directory.join("wireplumber"),
        )
        .env("WIREPLUMBER_MODULE_DIR", env.library_directory)
        .env("XDG_DATA_DIRS", env.share_directory)
        .env("LD_LIBRARY_PATH", env.library_path)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone()?))
        .stderr(Stdio::from(log.try_clone()?));
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
    command.spawn()
}

fn spa_libs_config() -> &'static str {
    r#"context.spa-libs = {
    support.* = support/libspa-support
    audio.convert.* = audioconvert/libspa-audioconvert
    audio.adapt = audioconvert/libspa-audioconvert
    audio.mixer.* = audiomixer/libspa-audiomixer
}
"#
}

fn write_pipewire_config(directory: &Path) -> io::Result<PathBuf> {
    let body = format!(
        r#"context.properties = {{
    core.daemon = true
    core.name = pipewire-0
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 48000 ]
    default.clock.quantum = 1024
    link.max-buffers = 16
    mem.warn-mlock = false
}}

{}

context.modules = [
    {{ name = libpipewire-module-rt flags = [ ifexists nofail ] }}
    {{ name = libpipewire-module-protocol-native }}
    {{ name = libpipewire-module-profiler flags = [ ifexists nofail ] }}
    {{ name = libpipewire-module-metadata }}
    {{ name = libpipewire-module-spa-device-factory }}
    {{ name = libpipewire-module-spa-node-factory }}
    {{ name = libpipewire-module-client-node }}
    {{ name = libpipewire-module-client-device }}
    {{ name = libpipewire-module-access args = {{
        access.socket = {{
            pipewire-0 = "unrestricted"
            pipewire-0-manager = "unrestricted"
        }}
    }} }}
    {{ name = libpipewire-module-adapter }}
    {{ name = libpipewire-module-link-factory }}
]

context.objects = [
    {{ factory = spa-node-factory
        args = {{
            factory.name = support.node.driver
            node.name = Dummy-Driver
            node.group = pipewire.dummy
            node.sync-group = sync.dummy
            priority.driver = 200000
        }}
    }}
    {{ factory = spa-node-factory
        args = {{
            factory.name = support.node.driver
            node.name = Freewheel-Driver
            priority.driver = 190000
            node.group = pipewire.freewheel
            node.sync-group = sync.dummy
            node.freewheel = true
        }}
    }}
]
"#,
        spa_libs_config()
    );
    let path = directory.join("pipewire.conf");
    fs::write(&path, body)?;
    Ok(path)
}

fn write_client_config(directory: &Path) -> io::Result<()> {
    let body = format!(
        "context.properties = {{ application.name = trierarch-pipewire-aaudio-sink }}\n{}\ncontext.modules = [ {{ name = libpipewire-module-protocol-native }} {{ name = libpipewire-module-client-node }} {{ name = libpipewire-module-adapter }} ]\n",
        spa_libs_config()
    );
    fs::write(directory.join("client.conf"), body)
}

fn write_pipewire_pulse_config(directory: &Path, runtime_directory: &Path) -> io::Result<PathBuf> {
    let socket = runtime_directory.join(PULSE_SOCKET);
    let body = format!(
        r#"context.properties = {{ mem.warn-mlock = false }}

{}

context.modules = [
    {{ name = libpipewire-module-rt flags = [ ifexists nofail ] }}
    {{ name = libpipewire-module-protocol-native }}
    {{ name = libpipewire-module-client-node }}
    {{ name = libpipewire-module-adapter }}
    {{ name = libpipewire-module-metadata }}
    {{ name = libpipewire-module-protocol-pulse }}
]

pulse.properties = {{
    server.address = [ "unix:{}" ]
    pulse.allow-module-loading = false
    pulse.min.req = 256/48000
    pulse.default.req = 960/48000
    pulse.min.quantum = 256/48000
    pulse.idle.timeout = 0
}}

stream.properties = {{ node.autoconnect = true resample.quality = 4 }}
pulse.cmd = [ ]
pulse.rules = [ ]
"#,
        spa_libs_config(),
        socket.display()
    );
    let path = directory.join("pipewire-pulse.conf");
    fs::write(&path, body)?;
    Ok(path)
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
        cleanup_sockets(&host.runtime_directory);
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

fn wait_for_socket(name: &str, child: &mut Child, path: &Path) -> io::Result<()> {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        chmod_socket(path);
        if UnixStream::connect(path).is_ok() {
            return Ok(());
        }
        if let Some(status) = child.try_wait()? {
            return Err(io::Error::other(format!(
                "{name} exited before creating {} (status {status})",
                path.display()
            )));
        }
        thread::sleep(Duration::from_millis(50));
    }
    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        format!("timed out waiting for {name} socket {}", path.display()),
    ))
}

fn ensure_child_running(name: &str, child: &mut Child) -> io::Result<()> {
    thread::sleep(Duration::from_millis(300));
    if let Some(status) = child.try_wait()? {
        return Err(io::Error::other(format!(
            "{name} exited during startup (status {status})"
        )));
    }
    Ok(())
}

fn cleanup_sockets(runtime_directory: &Path) {
    for name in [
        PIPEWIRE_SOCKET,
        "pipewire-0.lock",
        "pipewire-0-manager",
        "pipewire-0-manager.lock",
        PULSE_SOCKET,
        "pulse/native.lock",
    ] {
        let path = runtime_directory.join(name);
        if path.exists() {
            let _ = fs::remove_file(path);
        }
    }
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
    runtime_directory.join(PIPEWIRE_SOCKET)
}

fn pulse_socket_path(runtime_directory: &Path) -> PathBuf {
    runtime_directory.join(PULSE_SOCKET)
}

fn sockets_ready(runtime_directory: &Path) -> bool {
    is_socket(&socket_path(runtime_directory)) && is_socket(&pulse_socket_path(runtime_directory))
}
