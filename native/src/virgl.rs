//! Android-side VirGL vtest host lifecycle.
//!
//! The guest Mesa `virpipe` driver connects to `vtest.sock`; DroidSpaces bind
//! mounts the containing directory while it starts the container.

use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

const SOCKET_NAME: &str = "vtest.sock";
const SERVER_NAME: &str = "virgl_test_server_android";

static CHILD: OnceLock<Mutex<Option<Child>>> = OnceLock::new();

fn child_slot() -> &'static Mutex<Option<Child>> {
    CHILD.get_or_init(|| Mutex::new(None))
}

pub fn start(
    runtime_directory: &Path,
    payload_directory: &Path,
    native_library_directory: &Path,
) -> io::Result<()> {
    let mut slot = child_slot()
        .lock()
        .map_err(|_| io::Error::other("VirGL host state lock poisoned"))?;
    if let Some(child) = slot.as_mut() {
        if child.try_wait()?.is_none() && socket_path(runtime_directory).is_socket() {
            return Ok(());
        }
    }
    *slot = None;

    let server = payload_directory.join("bin").join(SERVER_NAME);
    if !server.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("VirGL host executable is missing: {}", server.display()),
        ));
    }
    fs::create_dir_all(runtime_directory)?;
    // This directory is bind-mounted into a DroidSpaces mount namespace whose
    // graphical client has a different UID from the Android app.  It must be
    // traversable to reach the known socket name, but need not be listable.
    fs::set_permissions(runtime_directory, fs::Permissions::from_mode(0o711))?;
    let socket = socket_path(runtime_directory);
    if socket.exists() {
        fs::remove_file(&socket)?;
    }

    let log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(runtime_directory.join("virgl_host.log"))?;
    let angle = payload_directory.join("angle/vulkan");
    let library = payload_directory.join("lib");
    let linker = if cfg!(target_pointer_width = "64") {
        "/system/bin/linker64"
    } else {
        "/system/bin/linker"
    };
    let mut command = Command::new(linker);
    command
        .arg(&server)
        .arg("--use-egl-surfaceless")
        .arg("--use-gles")
        .arg("--socket-path")
        .arg(&socket)
        .current_dir(runtime_directory)
        .env("XDG_RUNTIME_DIR", runtime_directory)
        .env("TMPDIR", runtime_directory)
        .env("ANGLE_LIBS_DIR", &angle)
        .env(
            "LD_LIBRARY_PATH",
            format!(
                "{}:{}:{}:{}:/system/lib64",
                library.display(),
                server.parent().unwrap_or(Path::new(".")).display(),
                angle.display(),
                native_library_directory.display(),
            ),
        )
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone()?))
        .stderr(Stdio::from(log));
    // virglrenderer creates the vtest socket with mode 0777.  Android apps
    // normally inherit a restrictive umask, which made it inaccessible to the
    // guest UID even after the directory was bind-mounted.  0111 yields a
    // connectable 0666 socket without making ordinary files executable.
    unsafe {
        command.pre_exec(|| {
            libc::umask(0o111);
            Ok(())
        });
    }
    let mut child = command.spawn()?;

    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline {
        if socket.is_socket() {
            *slot = Some(child);
            return Ok(());
        }
        if let Some(status) = child.try_wait()? {
            return Err(io::Error::other(format!(
                "VirGL host exited before creating its socket: {status}"
            )));
        }
        std::thread::sleep(Duration::from_millis(25));
    }
    let _ = child.kill();
    let _ = child.wait();
    Err(io::Error::new(
        io::ErrorKind::TimedOut,
        "Timed out waiting for the VirGL host socket",
    ))
}

pub fn stop() {
    if let Ok(mut slot) = child_slot().lock() {
        if let Some(mut child) = slot.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn socket_path(runtime_directory: &Path) -> PathBuf {
    runtime_directory.join(SOCKET_NAME)
}

trait SocketPath {
    fn is_socket(&self) -> bool;
}

impl SocketPath for PathBuf {
    fn is_socket(&self) -> bool {
        use std::os::unix::fs::FileTypeExt;
        self.symlink_metadata()
            .map(|metadata| metadata.file_type().is_socket())
            .unwrap_or(false)
    }
}
