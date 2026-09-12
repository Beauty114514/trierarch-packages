use crate::rootfs;
use anyhow::{bail, ensure, Context, Result};
use std::env;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

mod runtime_client;

const PROFILES_DIRECTORY: &str = "config/profiles";

pub fn run(arguments: Vec<std::ffi::OsString>) -> Result<()> {
    let files_directory = env::var_os("TRIERARCH_FILES_DIR")
        .map(PathBuf::from)
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
        [command] if command == "--status" => {
            runtime_client::request(&files_directory, "status", None)
        }
        [command, id] if command == "--status" => {
            validate_profile_id(id)?;
            runtime_client::request(&files_directory, "status", Some(id))
        }
        [command, id] if command == "--config" => edit_profile(&files_directory, id),
        [command, id] if command == "--delete" => delete_profile(&files_directory, id),
        [command, id] if command == "--run" => {
            validate_profile_id(id)?;
            runtime_client::request(&files_directory, "run", Some(id))
        }
        [command] if command == "--stop" => {
            runtime_client::request(&files_directory, "stop", None)
        }
        [command, id] if command == "--stop" => {
            validate_profile_id(id)?;
            runtime_client::request(&files_directory, "stop", Some(id))
        }
        [command, id] if command == "--rerun" => {
            validate_profile_id(id)?;
            runtime_client::request(&files_directory, "rerun", Some(id))
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

fn print_usage() -> Result<()> {
    println!(
        "usage:\n  trierarch --list\n  trierarch --status [ID]\n  trierarch --config ID\n  trierarch --delete ID\n  trierarch --run ID\n  trierarch --stop [ID]\n  trierarch --rerun ID\n  trierarch --import ARCHIVE.tar.xz --name NAME\n  trierarch --remove NAME"
    );
    Ok(())
}

fn profiles_directory(files_directory: &Path) -> PathBuf {
    files_directory.join(PROFILES_DIRECTORY)
}

fn profile_path(files_directory: &Path, id: &str) -> Result<PathBuf> {
    validate_profile_id(id)?;
    Ok(profiles_directory(files_directory).join(format!("{id}.toml")))
}

fn existing_profile_path(files_directory: &Path, id: &str) -> Result<PathBuf> {
    let profile = profile_path(files_directory, id)?;
    let metadata = fs::symlink_metadata(&profile)
        .with_context(|| format!("profile does not exist: {id}"))?;
    ensure!(
        metadata.file_type().is_file() && !metadata.file_type().is_symlink(),
        "profile is not a regular file: {id}"
    );
    Ok(profile)
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

fn list_profiles(files_directory: &Path) -> Result<()> {
    let directory = profiles_directory(files_directory);
    if !directory.exists() {
        println!("No profiles.");
        return Ok(());
    }
    ensure!(directory.is_dir(), "profile directory is not a directory");

    let mut profiles = fs::read_dir(&directory)
        .with_context(|| format!("list profiles in {}", directory.display()))?
        .filter_map(|entry| entry.ok())
        .filter_map(|entry| {
            let path = entry.path();
            (path.is_file() && path.extension().is_some_and(|extension| extension == "toml"))
                .then(|| path.file_stem().map(|name| name.to_owned()))
                .flatten()
        })
        .collect::<Vec<_>>();
    profiles.sort();
    if profiles.is_empty() {
        println!("No profiles.");
    } else {
        for profile in profiles {
            println!("{}", profile.to_string_lossy());
        }
    }
    Ok(())
}

fn edit_profile(files_directory: &Path, id: &str) -> Result<()> {
    let profile = existing_profile_path(files_directory, id)?;
    let editor = env::var_os("EDITOR").unwrap_or_else(|| "vi".into());
    let status = Command::new(&editor)
        .arg(&profile)
        .status()
        .with_context(|| format!("start editor {}", Path::new(&editor).display()))?;
    ensure!(status.success(), "editor exited with {status}");
    Ok(())
}

fn delete_profile(files_directory: &Path, id: &str) -> Result<()> {
    let profile = existing_profile_path(files_directory, id)?;
    confirm(&format!("Delete profile '{id}'?"))?;
    fs::remove_file(&profile).with_context(|| format!("delete profile {id}"))?;
    println!("Deleted profile: {id}");
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
