use anyhow::{bail, ensure, Context, Result};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::mem;
use std::os::fd::FromRawFd;
use std::path::Path;

const ENDPOINT_FILE: &str = "runtime/control";

pub(super) fn request(files_directory: &Path, operation: &str, id: Option<&str>) -> Result<()> {
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

    let mut request = format!("{token}\t{operation}");
    if let Some(id) = id {
        request.push('\t');
        request.push_str(id);
    }
    request.push('\n');
    let response = send(socket_name, request.as_bytes(), needs_acknowledgement(operation))?;
    let (kind, message) = response
        .trim_end()
        .split_once('\t')
        .context("invalid response from Trierarch runtime control")?;
    match kind {
        "OK" => {
            println!("{message}");
            Ok(())
        }
        "ERR" => bail!("{message}"),
        _ => bail!("invalid response from Trierarch runtime control"),
    }
}

fn send(socket_name: &str, request: &[u8], acknowledge: bool) -> Result<String> {
    let name = socket_name.as_bytes();
    ensure!(
        !name.is_empty() && name.len() + 1 < unix_socket_path_capacity(),
        "invalid Trierarch runtime socket name"
    );
    let descriptor = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM, 0) };
    if descriptor < 0 {
        return Err(std::io::Error::last_os_error()).context("create runtime control socket");
    }
    let mut stream = unsafe { std::fs::File::from_raw_fd(descriptor) };
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
    stream.write_all(request).context("send runtime control request")?;
    stream.flush().context("flush runtime control request")?;
    let response = if acknowledge {
        let mut response = String::new();
        BufReader::new(&mut stream)
            .read_line(&mut response)
            .context("read runtime control response")?;
        stream
            .write_all(b"ACK\n")
            .context("acknowledge runtime control response")?;
        stream
            .flush()
            .context("flush runtime control acknowledgement")?;
        response
    } else {
        let mut response = String::new();
        stream
            .read_to_string(&mut response)
            .context("read runtime control response")?;
        response
    };
    Ok(response)
}

fn needs_acknowledgement(operation: &str) -> bool {
    matches!(operation, "run" | "stop" | "rerun")
}

fn unix_socket_path_capacity() -> usize {
    let address: libc::sockaddr_un = unsafe { mem::zeroed() };
    address.sun_path.len()
}
