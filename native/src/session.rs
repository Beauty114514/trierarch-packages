use crate::{ChrootSpec, ProotSpec, proot, pty};
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;

pub type SessionId = i64;

const MIN_DIMENSION: i32 = 1;
const MAX_DIMENSION: i32 = u16::MAX as i32;
const READ_BUFFER_SIZE: usize = 16 * 1024;

/// An executable invocation independent of the runtime that produced it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LaunchSpec {
    pub command: PathBuf,
    pub arguments: Vec<String>,
    pub working_directory: PathBuf,
    /// `NAME=VALUE` entries. The complete environment is explicit.
    pub environment: Vec<String>,
}

impl LaunchSpec {
    pub fn validate(&self) -> io::Result<()> {
        if !self.command.is_absolute() {
            return invalid_input("command must be an absolute path");
        }
        if !self.working_directory.is_absolute() {
            return invalid_input("working directory must be an absolute path");
        }
        if self.command.as_os_str().as_encoded_bytes().contains(&0)
            || self
                .working_directory
                .as_os_str()
                .as_encoded_bytes()
                .contains(&0)
        {
            return invalid_input("paths must not contain NUL bytes");
        }
        for argument in &self.arguments {
            if argument.as_bytes().contains(&0) {
                return invalid_input("arguments must not contain NUL bytes");
            }
        }
        for entry in &self.environment {
            let Some((name, _)) = entry.split_once('=') else {
                return invalid_input("environment entries must use NAME=VALUE form");
            };
            if name.is_empty() || name.as_bytes().contains(&0) || entry.as_bytes().contains(&0) {
                return invalid_input("environment entry is invalid");
            }
        }
        Ok(())
    }
}

pub trait SessionEvents: Send + Sync + 'static {
    fn on_output(&self, session_id: SessionId, bytes: &[u8]);
    fn on_exited(&self, session_id: SessionId, result: SessionResult);
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SessionResult {
    Exited(i32),
    Signaled(i32),
    Unknown,
}

impl SessionResult {
    pub fn exit_code(self) -> i32 {
        match self {
            Self::Exited(code) => code,
            Self::Signaled(signal) => 128 + signal,
            Self::Unknown => -1,
        }
    }
}

pub struct SessionManager {
    next_id: AtomicI64,
    sessions: Arc<Mutex<HashMap<SessionId, Arc<Session>>>>,
}

struct Session {
    pid: libc::pid_t,
    writer: Mutex<Option<Box<dyn Write + Send>>>,
    master_fd: RawFd,
    proot_child: Mutex<Option<proot::ChildProcess>>,
    closed: AtomicBool,
}

impl Default for SessionManager {
    fn default() -> Self {
        Self::new()
    }
}

impl SessionManager {
    pub fn new() -> Self {
        Self {
            next_id: AtomicI64::new(1),
            sessions: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    pub fn open(
        &self,
        spec: LaunchSpec,
        rows: i32,
        columns: i32,
        events: Arc<dyn SessionEvents>,
    ) -> io::Result<SessionId> {
        spec.validate()?;
        let (rows, columns) = dimensions(rows, columns)?;
        let spawned = pty::spawn(
            &spec.command,
            &spec.arguments,
            &spec.working_directory,
            &spec.environment,
            rows,
            columns,
        )?;
        self.open_spawned(spawned, events, true)
    }

    pub fn open_proot(
        &self,
        spec: ProotSpec,
        rows: i32,
        columns: i32,
        events: Arc<dyn SessionEvents>,
    ) -> io::Result<SessionId> {
        let (rows, columns) = dimensions(rows, columns)?;
        let (child, reader, writer, master_fd) = spec
            .fork_pty_shell(rows, columns)
            .map_err(io::Error::other)?;
        self.open_proot_spawned(child, reader, writer, master_fd, events)
    }

    pub fn open_chroot(
        &self,
        spec: ChrootSpec,
        rows: i32,
        columns: i32,
        events: Arc<dyn SessionEvents>,
    ) -> io::Result<SessionId> {
        self.open(spec.launch_spec()?, rows, columns, events)
    }

    fn open_spawned(
        &self,
        spawned: pty::SpawnedPty,
        events: Arc<dyn SessionEvents>,
        reap_child: bool,
    ) -> io::Result<SessionId> {
        let master_fd = spawned.master.as_raw_fd();
        let reader = duplicate_fd(spawned.master.as_raw_fd())?;
        let id = self.next_session_id();
        let session = Arc::new(Session {
            pid: spawned.pid,
            writer: Mutex::new(Some(Box::new(std::fs::File::from(spawned.master)))),
            master_fd,
            proot_child: Mutex::new(None),
            closed: AtomicBool::new(false),
        });
        self.sessions
            .lock()
            .expect("session registry poisoned")
            .insert(id, session.clone());

        spawn_reader(id, reader, events.clone());
        if reap_child {
            spawn_reaper(id, spawned.pid, session, self.sessions.clone(), events);
        }
        Ok(id)
    }

    fn open_proot_spawned(
        &self,
        child: proot::ChildProcess,
        reader: std::fs::File,
        writer: Box<dyn Write + Send>,
        master_fd: RawFd,
        events: Arc<dyn SessionEvents>,
    ) -> io::Result<SessionId> {
        let id = self.next_session_id();
        let session = Arc::new(Session {
            pid: child.pid.as_raw(),
            writer: Mutex::new(Some(writer)),
            master_fd,
            proot_child: Mutex::new(Some(child)),
            closed: AtomicBool::new(false),
        });
        self.sessions
            .lock()
            .expect("session registry poisoned")
            .insert(id, session);
        spawn_reader_file(id, reader, events);
        Ok(id)
    }

    pub fn write(&self, id: SessionId, bytes: &[u8]) -> io::Result<()> {
        let session = self.get_open_session(id)?;
        let mut writer = session.writer.lock().expect("session writer poisoned");
        let fd = writer.as_mut().ok_or_else(session_closed)?;
        fd.write_all(bytes)
    }

    pub fn resize(&self, id: SessionId, rows: i32, columns: i32) -> io::Result<()> {
        let (rows, columns) = dimensions(rows, columns)?;
        let session = self.get_open_session(id)?;
        pty::resize(session.master_fd, rows, columns)
    }

    pub fn close(&self, id: SessionId) -> bool {
        let session = self
            .sessions
            .lock()
            .expect("session registry poisoned")
            .remove(&id);
        let Some(session) = session else {
            return false;
        };
        session.close();
        true
    }

    fn get_open_session(&self, id: SessionId) -> io::Result<Arc<Session>> {
        let session = self
            .sessions
            .lock()
            .expect("session registry poisoned")
            .get(&id)
            .cloned()
            .ok_or_else(session_closed)?;
        if session.closed.load(Ordering::Acquire) {
            return Err(session_closed());
        }
        Ok(session)
    }

    fn next_session_id(&self) -> SessionId {
        self.next_id.fetch_add(1, Ordering::Relaxed)
    }
}

impl Session {
    fn close(&self) {
        if self.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        if self
            .proot_child
            .lock()
            .expect("session child poisoned")
            .take()
            .is_none()
        {
            // The generic command path owns a process group.
            unsafe {
                libc::kill(-self.pid, libc::SIGHUP);
                libc::kill(-self.pid, libc::SIGTERM);
            }
        }
        self.writer.lock().expect("session writer poisoned").take();
    }
}

impl Drop for SessionManager {
    fn drop(&mut self) {
        let sessions =
            std::mem::take(&mut *self.sessions.lock().expect("session registry poisoned"));
        for session in sessions.into_values() {
            session.close();
        }
    }
}

fn spawn_reader(id: SessionId, reader: OwnedFd, events: Arc<dyn SessionEvents>) {
    spawn_reader_file(id, std::fs::File::from(reader), events);
}

fn spawn_reader_file(id: SessionId, reader: std::fs::File, events: Arc<dyn SessionEvents>) {
    thread::spawn(move || {
        let mut reader = reader;
        let mut buffer = [0_u8; READ_BUFFER_SIZE];
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => return,
                Ok(count) => events.on_output(id, &buffer[..count]),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                // Linux PTYs commonly report EIO once their slave closes.
                Err(_) => return,
            }
        }
    });
}

fn spawn_reaper(
    id: SessionId,
    pid: libc::pid_t,
    session: Arc<Session>,
    sessions: Arc<Mutex<HashMap<SessionId, Arc<Session>>>>,
    events: Arc<dyn SessionEvents>,
) {
    thread::spawn(move || {
        let result = wait_for_child(pid);
        session.closed.store(true, Ordering::Release);
        session
            .writer
            .lock()
            .expect("session writer poisoned")
            .take();
        let mut registry = sessions.lock().expect("session registry poisoned");
        if registry
            .get(&id)
            .is_some_and(|registered| Arc::ptr_eq(registered, &session))
        {
            registry.remove(&id);
        }
        drop(registry);
        events.on_exited(id, result);
    });
}

fn wait_for_child(pid: libc::pid_t) -> SessionResult {
    let mut status = 0;
    loop {
        // SAFETY: pid is the child created by this session and status is valid output storage.
        let result = unsafe { libc::waitpid(pid, &mut status, 0) };
        if result == pid {
            if libc::WIFEXITED(status) {
                return SessionResult::Exited(libc::WEXITSTATUS(status));
            }
            if libc::WIFSIGNALED(status) {
                return SessionResult::Signaled(libc::WTERMSIG(status));
            }
            return SessionResult::Unknown;
        }
        if result < 0 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
            continue;
        }
        return SessionResult::Unknown;
    }
}

fn duplicate_fd(fd: i32) -> io::Result<OwnedFd> {
    // SAFETY: fd is a live PTY master owned by the newly-created session.
    let duplicated = unsafe { libc::dup(fd) };
    if duplicated < 0 {
        Err(io::Error::last_os_error())
    } else {
        // SAFETY: dup returned a distinct owned descriptor.
        Ok(unsafe { OwnedFd::from_raw_fd(duplicated) })
    }
}

fn dimensions(rows: i32, columns: i32) -> io::Result<(u16, u16)> {
    if !(MIN_DIMENSION..=MAX_DIMENSION).contains(&rows)
        || !(MIN_DIMENSION..=MAX_DIMENSION).contains(&columns)
    {
        return invalid_input("terminal dimensions must be between 1 and 65535");
    }
    Ok((rows as u16, columns as u16))
}

fn invalid_input<T>(message: &str) -> io::Result<T> {
    Err(io::Error::new(io::ErrorKind::InvalidInput, message))
}

fn session_closed() -> io::Error {
    io::Error::new(
        io::ErrorKind::NotFound,
        "session is closed or does not exist",
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Condvar;
    use std::time::{Duration, Instant};

    #[derive(Default)]
    struct RecordingEvents {
        state: Mutex<(Vec<u8>, Option<SessionResult>)>,
        changed: Condvar,
    }

    impl SessionEvents for RecordingEvents {
        fn on_output(&self, _: SessionId, bytes: &[u8]) {
            let mut state = self.state.lock().expect("test state poisoned");
            state.0.extend_from_slice(bytes);
            self.changed.notify_all();
        }

        fn on_exited(&self, _: SessionId, result: SessionResult) {
            let mut state = self.state.lock().expect("test state poisoned");
            state.1 = Some(result);
            self.changed.notify_all();
        }
    }

    impl RecordingEvents {
        fn wait_for_exit_and_output(&self, expected_output: &[u8]) -> (Vec<u8>, SessionResult) {
            let deadline = Instant::now() + Duration::from_secs(2);
            let mut state = self.state.lock().expect("test state poisoned");
            loop {
                if state.1.is_some()
                    && state
                        .0
                        .windows(expected_output.len())
                        .any(|window| window == expected_output)
                {
                    return (state.0.clone(), state.1.expect("exit checked above"));
                }
                let remaining = deadline.saturating_duration_since(Instant::now());
                assert!(!remaining.is_zero(), "timed out waiting for PTY session");
                let (next, _) = self
                    .changed
                    .wait_timeout(state, remaining)
                    .expect("test state poisoned");
                state = next;
            }
        }
    }

    fn valid_spec() -> LaunchSpec {
        LaunchSpec {
            command: PathBuf::from("/bin/sh"),
            arguments: vec!["-i".into()],
            working_directory: PathBuf::from("/tmp"),
            environment: vec!["HOME=/tmp".into(), "TERM=xterm-256color".into()],
        }
    }

    #[test]
    fn accepts_a_complete_launch_spec() {
        assert!(valid_spec().validate().is_ok());
    }

    #[test]
    fn rejects_relative_paths() {
        let mut spec = valid_spec();
        spec.command = PathBuf::from("sh");
        assert!(spec.validate().is_err());
        spec.command = PathBuf::from("/bin/sh");
        spec.working_directory = PathBuf::from("workspace");
        assert!(spec.validate().is_err());
    }

    #[test]
    fn rejects_malformed_environment() {
        let mut spec = valid_spec();
        spec.environment = vec!["HOME".into()];
        assert!(spec.validate().is_err());
    }

    #[test]
    fn validates_terminal_dimensions() {
        assert!(dimensions(24, 80).is_ok());
        assert!(dimensions(0, 80).is_err());
        assert!(dimensions(24, 65_536).is_err());
    }

    #[test]
    fn runs_a_program_in_a_pty_and_reaps_it() {
        let manager = SessionManager::new();
        let events = Arc::new(RecordingEvents::default());
        let spec = LaunchSpec {
            command: PathBuf::from("/bin/sh"),
            arguments: vec!["-c".into(), "printf ready; exit 7".into()],
            working_directory: PathBuf::from("/tmp"),
            environment: vec!["PATH=/usr/bin:/bin".into(), "TERM=xterm-256color".into()],
        };

        let session_id = manager
            .open(spec, 24, 80, events.clone())
            .expect("open PTY session");
        assert!(session_id > 0);
        let (output, result) = events.wait_for_exit_and_output(b"ready");
        assert!(output.windows(5).any(|window| window == b"ready"));
        assert_eq!(result, SessionResult::Exited(7));
        assert!(!manager.close(session_id));
    }

    #[test]
    fn writes_input_and_resizes_a_live_pty() {
        let manager = SessionManager::new();
        let events = Arc::new(RecordingEvents::default());
        let spec = LaunchSpec {
            command: PathBuf::from("/bin/sh"),
            arguments: vec![
                "-c".into(),
                "read line; printf received:%s \"$line\"".into(),
            ],
            working_directory: PathBuf::from("/tmp"),
            environment: vec!["PATH=/usr/bin:/bin".into(), "TERM=xterm-256color".into()],
        };

        let session_id = manager
            .open(spec, 24, 80, events.clone())
            .expect("open PTY session");
        manager.resize(session_id, 40, 120).expect("resize PTY");
        manager
            .write(session_id, b"hello\n")
            .expect("write PTY input");
        let (output, result) = events.wait_for_exit_and_output(b"received:hello");
        assert!(output.windows(14).any(|window| window == b"received:hello"));
        assert_eq!(result, SessionResult::Exited(0));
    }
}
