use anyhow::{bail, ensure, Context, Result};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::mem;
use std::os::fd::FromRawFd;
use std::path::Path;

const ENDPOINT_FILE: &str = "runtime/control";
const MAX_FRAME_BYTES: usize = 1 << 20;

/// A structured failure returned by the app's core.
#[derive(Debug)]
pub(super) struct ControlError {
    code: String,
    message: String,
}

impl ControlError {
    pub(super) fn exit_code(&self) -> i32 {
        match self.code.as_str() {
            "already_running" => 2,
            "display_busy" => 3,
            "not_running" | "no_session" => 5,
            "invalid_document" => 6,
            "revision_mismatch" => 7,
            "busy" => 8,
            "already_exists" => 9,
            "not_found" => 10,
            _ => 1,
        }
    }
}

impl std::fmt::Display for ControlError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for ControlError {}

pub(super) struct Response {
    pub(super) message: String,
    pub(super) revision: Option<String>,
    pub(super) text: Option<String>,
}

pub(super) fn request(files_directory: &Path, method: &str, arguments: &[&str]) -> Result<Response> {
    let endpoint = fs::read_to_string(files_directory.join(ENDPOINT_FILE))
        .context("read Trierarch runtime control endpoint")?;
    let (socket_name, token) = endpoint
        .trim_end()
        .split_once('\t')
        .context("invalid Trierarch runtime control endpoint")?;
    ensure!(
        !socket_name.is_empty() && !token.is_empty(),
        "invalid Trierarch runtime control endpoint"
    );

    let mut payload = format!("{token}\t{method}");
    for argument in arguments {
        payload.push('\t');
        payload.push_str(argument);
    }

    let mut stream = connect(socket_name)?;
    write_frame(&mut stream, &payload)?;
    let response = read_frame(&mut stream)?;
    if matches!(method, "run" | "stop" | "open") {
        stream
            .write_all(b"ACK\n")
            .context("acknowledge runtime control response")?;
        stream
            .flush()
            .context("flush runtime control acknowledgement")?;
    }
    parse_response(method, response)
}

fn parse_response(method: &str, payload: String) -> Result<Response> {
    let (kind, rest) = payload
        .split_once('\t')
        .context("invalid response from Trierarch runtime control")?;
    match kind {
        "OK" if method == "documents.read" => {
            let (revision, text) = rest
                .split_once('\t')
                .context("invalid document response from Trierarch")?;
            Ok(Response {
                message: String::new(),
                revision: Some(revision.to_owned()),
                text: Some(text.to_owned()),
            })
        }
        "OK" => Ok(Response {
            message: rest.to_owned(),
            revision: None,
            text: None,
        }),
        "ERR" => {
            let (code, message) = rest
                .split_once('\t')
                .context("invalid error response from Trierarch")?;
            Err(ControlError {
                code: code.to_owned(),
                message: message.to_owned(),
            }
            .into())
        }
        _ => bail!("invalid response from Trierarch runtime control"),
    }
}

fn connect(socket_name: &str) -> Result<std::fs::File> {
    let name = socket_name.as_bytes();
    ensure!(
        !name.is_empty() && name.len() + 1 < unix_socket_path_capacity(),
        "invalid Trierarch runtime socket name"
    );
    let descriptor = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
    if descriptor < 0 {
        return Err(std::io::Error::last_os_error()).context("create runtime control socket");
    }
    let mut address: libc::sockaddr_un = unsafe { mem::zeroed() };
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (index, byte) in name.iter().enumerate() {
        address.sun_path[index + 1] = *byte as libc::c_char;
    }
    let address_length = (mem::size_of::<libc::sa_family_t>() + 1 + name.len()) as libc::socklen_t;
    let connected = unsafe {
        libc::connect(
            descriptor,
            &address as *const libc::sockaddr_un as *const libc::sockaddr,
            address_length,
        )
    };
    if connected < 0 {
        return Err(std::io::Error::last_os_error()).context("connect to Trierarch runtime control");
    }
    Ok(unsafe { std::fs::File::from_raw_fd(descriptor) })
}

fn write_frame(stream: &mut std::fs::File, payload: &str) -> Result<()> {
    let bytes = payload.as_bytes();
    write!(stream, "{}\n", bytes.len()).context("write runtime control frame length")?;
    stream
        .write_all(bytes)
        .context("write runtime control request")?;
    stream.flush().context("flush runtime control request")?;
    Ok(())
}

fn read_frame(stream: &mut std::fs::File) -> Result<String> {
    let mut reader = BufReader::new(&mut *stream);
    let mut length = String::new();
    reader
        .read_line(&mut length)
        .context("read runtime control frame length")?;
    let size: usize = length
        .trim()
        .parse()
        .context("invalid runtime control frame length")?;
    ensure!(
        size <= MAX_FRAME_BYTES,
        "runtime control response is too large"
    );
    let mut bytes = vec![0u8; size];
    reader
        .read_exact(&mut bytes)
        .context("read runtime control response")?;
    String::from_utf8(bytes).context("runtime control response is not UTF-8")
}

fn unix_socket_path_capacity() -> usize {
    let address: libc::sockaddr_un = unsafe { mem::zeroed() };
    address.sun_path.len()
}
