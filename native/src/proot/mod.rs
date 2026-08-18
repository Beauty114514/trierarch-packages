//! Spawn the PRoot shell under a PTY.
//!
//! This is the minimal extraction of Trierarch-old's
//! `android/proot/mod.rs`: one PRoot process owns one PTY session.

use anyhow::{Context, Result};
use nix::pty::{ForkptyResult, Winsize, forkpty};
use nix::unistd::{Pid, dup, execve};
use std::io::Write;
use std::os::fd::IntoRawFd;
use std::os::unix::io::{FromRawFd, RawFd};
use std::path::PathBuf;

mod args;

#[derive(Clone, Debug)]
pub struct ProotSpec {
    pub rootfs: PathBuf,
    /// Absolute path as seen from inside the guest rootfs, e.g. `/bin/bash`.
    pub shell: PathBuf,
    pub native_library_dir: PathBuf,
    pub cache_dir: PathBuf,
    /// Empty means terminal-only; otherwise bind this host directory's X0 socket.
    pub x11_socket_directory: PathBuf,
    /// Empty starts the configured interactive shell.
    pub launch_argv: Vec<String>,
}

pub struct ChildProcess {
    pub pid: Pid,
}

impl Drop for ChildProcess {
    fn drop(&mut self) {
        let _ = nix::sys::signal::kill(self.pid, nix::sys::signal::Signal::SIGTERM);
    }
}

impl ProotSpec {
    /// One interactive PRoot shell on a new PTY. The caller owns all session
    /// registration and reader/writer lifecycle.
    pub(crate) fn fork_pty_shell(
        &self,
        initial_rows: u16,
        initial_cols: u16,
    ) -> Result<(ChildProcess, std::fs::File, Box<dyn Write + Send>, RawFd)> {
        let (argv, env) = args::build_exec_args(self)?;
        let argv_refs: Vec<&std::ffi::CStr> = argv.iter().map(|value| value.as_c_str()).collect();
        let env_refs: Vec<&std::ffi::CStr> = env.iter().map(|value| value.as_c_str()).collect();

        let winsize = Winsize {
            ws_row: initial_rows.max(1),
            ws_col: initial_cols.max(1),
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        let result = unsafe { forkpty(Some(&winsize), None).context("forkpty failed")? };

        match result {
            ForkptyResult::Child => {
                if execve(argv[0].as_c_str(), &argv_refs, &env_refs).is_err() {
                    unsafe { nix::libc::_exit(1) };
                }
                unreachable!();
            }
            ForkptyResult::Parent { child, master } => {
                let master_read_fd = dup(&master).context("dup master for read")?.into_raw_fd();
                let master_write_fd = master.into_raw_fd();
                let master_read = unsafe { std::fs::File::from_raw_fd(master_read_fd) };
                let master_write = unsafe { std::fs::File::from_raw_fd(master_write_fd) };
                Ok((
                    ChildProcess { pid: child },
                    master_read,
                    Box::new(master_write),
                    master_write_fd,
                ))
            }
        }
    }
}
