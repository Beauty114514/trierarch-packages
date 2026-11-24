use crate::rootfs;
use anyhow::{bail, ensure, Context, Result};
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::Path;
use std::process::Command;

mod runtime_client;

use runtime_client::Response;

pub fn run(arguments: Vec<std::ffi::OsString>) -> Result<()> {
    let files_directory = env::var_os("TRIERARCH_FILES_DIR")
        .map(std::path::PathBuf::from)
        .ok_or_else(|| anyhow::anyhow!("TRIERARCH_FILES_DIR is not set"))?;
    let arguments: Vec<String> = arguments
        .into_iter()
        .map(|argument| {
            argument
                .into_string()
                .map_err(|_| anyhow::anyhow!("arguments must be UTF-8"))
        })
        .collect::<Result<_>>()?;

    match arguments.as_slice() {
        [command] if command == "--help" => print_usage(),
        [command] if command == "--list" => list_profiles(&files_directory),
        [command] if command == "--gui" => {
            print_response(runtime_client::request(&files_directory, "gui.open", &[])?)
        }
        [command] if command == "--status" => {
            print_response(runtime_client::request(&files_directory, "status", &[])?)
        }
        [command, id] if command == "--status" => {
            validate_profile_id(id)?;
            print_response(runtime_client::request(&files_directory, "status", &[id])?)
        }
        [command, id] if command == "--create" => create_profile(&files_directory, id),
        [command, id] if command == "--config" => edit_profile(&files_directory, id),
        [command, id] if command == "--delete" => delete_profile(&files_directory, id),
        [command, id] if command == "--run" => {
            validate_profile_id(id)?;
            print_response(runtime_client::request(&files_directory, "run", &[id])?)
        }
        [command, id] if command == "--open" => {
            validate_profile_id(id)?;
            print_response(runtime_client::request(&files_directory, "open", &[id])?)
        }
        [command] if command == "--stop" => {
            print_response(runtime_client::request(&files_directory, "stop", &[])?)
        }
        [command, id] if command == "--stop" => {
            validate_profile_id(id)?;
            print_response(runtime_client::request(&files_directory, "stop", &[id])?)
        }
        [command, archive, name_flag, name]
            if command == "--import" && name_flag == "--name" =>
        {
            let installed = rootfs::import_xz_tar(Path::new(archive), &files_directory, name)?;
            println!("Imported PRoot environment: {}", installed.display());
            Ok(())
        }
        [command, name] if command == "--remove" => remove_proot(&files_directory, name),
        _ => {
            print_usage()?;
            bail!("invalid arguments")
        }
    }
}

/// Maps a typed core failure to the process exit code the CLI reports.
pub fn exit_code(error: &anyhow::Error) -> i32 {
    error
        .downcast_ref::<runtime_client::ControlError>()
        .map(runtime_client::ControlError::exit_code)
        .unwrap_or(1)
}

fn print_response(response: Response) -> Result<()> {
    println!("{}", response.message);
    Ok(())
}

fn print_usage() -> Result<()> {
    println!(
        "usage:\n  trierarch --list\n  trierarch --status [ID]\n  trierarch --gui\n  trierarch --create ID\n  trierarch --config ID\n  trierarch --delete ID\n  trierarch --run ID\n  trierarch --open ID\n  trierarch --stop [ID]\n  trierarch --import ARCHIVE.tar.xz --name NAME\n  trierarch --remove NAME"
    );
    Ok(())
}

fn list_profiles(files_directory: &Path) -> Result<()> {
    let response = runtime_client::request(files_directory, "documents.list", &[])?;
    if response.message.is_empty() {
        println!("No profiles.");
    } else {
        println!("{}", response.message);
    }
    Ok(())
}

fn create_profile(files_directory: &Path, id: &str) -> Result<()> {
    validate_profile_id(id)?;
    let response = runtime_client::request(files_directory, "documents.create", &[id])?;
    println!("{}", response.message);
    println!("Edit it with: trierarch --config {id}");
    Ok(())
}

fn edit_profile(files_directory: &Path, id: &str) -> Result<()> {
    validate_profile_id(id)?;
    let document = runtime_client::request(files_directory, "documents.read", &[id])?;
    let revision = document
        .revision
        .context("Trierarch returned no profile revision")?;
    let original = document
        .text
        .context("Trierarch returned no profile text")?;
    let temporary = env::temp_dir().join(format!("trierarch-{id}-{}.toml", std::process::id()));
    fs::write(&temporary, original)
        .with_context(|| format!("create editable copy {}", temporary.display()))?;
    let editor = env::var_os("EDITOR").unwrap_or_else(|| "vi".into());
    let status = Command::new(&editor)
        .arg(&temporary)
        .status()
        .map_err(|error| {
            anyhow::anyhow!(
                "could not start editor {}: {error}; the original profile was not changed\nEdited copy kept at: {}",
                Path::new(&editor).display(),
                temporary.display(),
            )
        })?;
    ensure!(
        status.success(),
        "editor exited with {status}; the original profile was not changed\nEdited copy kept at: {}",
        temporary.display(),
    );

    let edited = fs::read_to_string(&temporary)
        .with_context(|| format!("read edited profile {}", temporary.display()))?;
    let saved = runtime_client::request(
        files_directory,
        "documents.save",
        &[id, revision.as_str(), edited.as_str()],
    )
    .map_err(|error| {
        error.context(format!(
            "Configuration was not saved; edited copy kept at: {}",
            temporary.display(),
        ))
    })?;
    let _ = fs::remove_file(&temporary);
    println!("{}", saved.message);
    Ok(())
}

fn delete_profile(files_directory: &Path, id: &str) -> Result<()> {
    validate_profile_id(id)?;
    let document = runtime_client::request(files_directory, "documents.read", &[id])?;
    let revision = document
        .revision
        .context("Trierarch returned no profile revision")?;
    confirm(&format!("Delete profile '{id}'?"))?;
    let response = runtime_client::request(
        files_directory,
        "documents.delete",
        &[id, revision.as_str()],
    )?;
    println!("{}", response.message);
    Ok(())
}

fn validate_profile_id(id: &str) -> Result<()> {
    let valid = !id.is_empty()
        && id.len() <= 64
        && id.as_bytes()[0].is_ascii_lowercase()
        && id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'-');
    ensure!(
        valid,
        "profile id must use lowercase English letters, digits, '-' and start with a letter"
    );
    Ok(())
}

fn remove_proot(files_directory: &Path, name: &str) -> Result<()> {
    rootfs::validate_name(name)?;
    confirm(&format!("Remove PRoot environment '{name}' and all of its data?"))?;
    rootfs::remove_imported(files_directory, name)?;
    println!("Removed PRoot environment: {name}");
    Ok(())
}

fn confirm(question: &str) -> Result<()> {
    eprint!("{question} [y/N] ");
    io::stderr().flush().context("flush confirmation prompt")?;
    let mut answer = String::new();
    io::stdin().read_line(&mut answer).context("read confirmation")?;
    ensure!(
        matches!(answer.trim(), "y" | "Y" | "yes" | "YES"),
        "cancelled"
    );
    Ok(())
}
