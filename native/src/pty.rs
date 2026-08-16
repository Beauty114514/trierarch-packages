use std::ffi::{CString, NulError};
use std::io;
use std::os::fd::{FromRawFd, OwnedFd, RawFd};
use std::path::Path;

#[derive(Debug)]
pub(crate) struct SpawnedPty {
    pub(crate) pid: libc::pid_t,
    pub(crate) master: OwnedFd,
}

struct PreparedCommand {
    command: CString,
    cwd: Option<CString>,
    // These allocations own the C strings referenced by the pointer arrays.
    _argv: Vec<CString>,
    argv_pointers: Vec<*const libc::c_char>,
    _environment: Vec<CString>,
    environment_pointers: Vec<*const libc::c_char>,
}

pub(crate) fn spawn(
    command: &Path,
    arguments: &[String],
    working_directory: &Path,
    environment: &[String],
    rows: u16,
    columns: u16,
) -> io::Result<SpawnedPty> {
    let prepared = PreparedCommand::new(command, arguments, Some(working_directory), environment)?;
    spawn_prepared(prepared, rows, columns)
}

/// PRoot establishes the guest working directory itself. The old native
/// backend therefore executed it without changing the host process cwd.
pub(crate) fn spawn_without_chdir(
    command: &Path,
    arguments: &[String],
    environment: &[String],
    rows: u16,
    columns: u16,
) -> io::Result<SpawnedPty> {
    let prepared = PreparedCommand::new(command, arguments, None, environment)?;
    spawn_prepared(prepared, rows, columns)
}

fn spawn_prepared(prepared: PreparedCommand, rows: u16, columns: u16) -> io::Result<SpawnedPty> {
    let mut master = -1;
    let window_size = libc::winsize {
        ws_row: rows,
        ws_col: columns,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };

    // `forkpty()` is the PTY creation path used by Trierarch's previous
    // native runtime. Besides opening the pair, it makes the child a session
    // leader and establishes the slave as its controlling terminal before
    // returning. PRoot is sensitive to that process/terminal setup.
    // SAFETY: pointers are valid for the duration of this call.
    let pid = unsafe {
        libc::forkpty(
            &mut master,
            std::ptr::null_mut(),
            std::ptr::null(),
            &window_size,
        )
    };
    if pid < 0 {
        return Err(io::Error::last_os_error());
    }
    if pid == 0 {
        child_exec(&prepared);
    }

    // SAFETY: master is unique and remains owned by the parent.
    let master = unsafe { OwnedFd::from_raw_fd(master) };
    Ok(SpawnedPty { pid, master })
}

impl PreparedCommand {
    fn new(
        command: &Path,
        arguments: &[String],
        working_directory: Option<&Path>,
        environment: &[String],
    ) -> io::Result<Self> {
        let command = to_c_string(command)?;
        let cwd = working_directory.map(to_c_string).transpose()?;
        let mut argv = Vec::with_capacity(arguments.len() + 1);
        argv.push(CString::new(command.as_bytes()).expect("command was already validated"));
        argv.extend(
            arguments
                .iter()
                .map(|value| to_c_string_value(value))
                .collect::<io::Result<Vec<_>>>()?,
        );
        let mut argv_pointers = argv.iter().map(|value| value.as_ptr()).collect::<Vec<_>>();
        argv_pointers.push(std::ptr::null());

        let environment = environment
            .iter()
            .map(|value| to_c_string_value(value))
            .collect::<io::Result<Vec<_>>>()?;
        let mut environment_pointers = environment
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        environment_pointers.push(std::ptr::null());

        Ok(Self {
            command,
            cwd,
            _argv: argv,
            argv_pointers,
            _environment: environment,
            environment_pointers,
        })
    }
}

fn to_c_string(path: &Path) -> io::Result<CString> {
    CString::new(path.as_os_str().as_encoded_bytes()).map_err(|_: NulError| {
        io::Error::new(io::ErrorKind::InvalidInput, "path contains a NUL byte")
    })
}

fn to_c_string_value(value: &str) -> io::Result<CString> {
    CString::new(value)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "string contains a NUL byte"))
}

fn child_exec(command: &PreparedCommand) -> ! {
    // After fork, do not acquire Rust locks or allocate. All data needed for
    // execve was prepared in the parent. `forkpty()` already configured the
    // controlling terminal and standard streams for this child.
    unsafe {
        if let Some(cwd) = &command.cwd {
            if libc::chdir(cwd.as_ptr()) != 0 {
                libc::_exit(127);
            }
        }
        libc::execve(
            command.command.as_ptr(),
            command.argv_pointers.as_ptr(),
            command.environment_pointers.as_ptr(),
        );
        libc::_exit(127);
    }
}

pub(crate) fn resize(fd: RawFd, rows: u16, columns: u16) -> io::Result<()> {
    let window_size = libc::winsize {
        ws_row: rows,
        ws_col: columns,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    // SAFETY: fd is a live PTY master while its session is in the registry.
    if unsafe { libc::ioctl(fd, libc::TIOCSWINSZ, &window_size) } == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}
