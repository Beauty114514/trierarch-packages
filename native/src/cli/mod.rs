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
        [command, id] if command == "--create" => create_profile(&files_directory, id),
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
        "usage:\n  trierarch --list\n  trierarch --status [ID]\n  trierarch --create ID\n  trierarch --config ID\n  trierarch --delete ID\n  trierarch --run ID\n  trierarch --stop [ID]\n  trierarch --rerun ID\n  trierarch --import ARCHIVE.tar.xz --name NAME\n  trierarch --remove NAME"
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

fn create_profile(files_directory: &Path, id: &str) -> Result<()> {
    let profile = profile_path(files_directory, id)?;
    let directory = profiles_directory(files_directory);
    fs::create_dir_all(&directory)
        .with_context(|| format!("create profile directory {}", directory.display()))?;
    ensure!(directory.is_dir(), "profile directory is not a directory");
    ensure!(!profile.exists(), "profile already exists: {id}");

    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&profile)
        .with_context(|| format!("create profile {id}"))?;
    write!(
        file,
        "# Complete this template before running the profile.\n\
         # Supported runtimes: proot, chroot, droidspaces.\n\
         id = \"{id}\"\n\
         runtime = \"proot\"\n\
         \n\
         # PRoot and chroot require these absolute guest paths.\n\
         rootfs = \"/absolute/path/to/rootfs\"\n\
         shell = \"/bin/bash\"\n\
         \n\
         # DroidSpaces instead requires: container = \"container-name\"\n\
         \n\
         [display]\n\
         type = \"none\"\n"
    )
    .with_context(|| format!("write profile {id}"))?;
    println!("Created profile: {id}\nEdit it with: trierarch --config {id}");
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
    let temporary = profiles_directory(files_directory).join(format!(".{id}.edit.tmp"));
    ensure!(
        !temporary.exists(),
        "an unsaved edited copy already exists: {}; inspect or remove it before editing again",
        temporary.display(),
    );
    fs::copy(&profile, &temporary)
        .with_context(|| format!("create editable copy for profile {id}"))?;
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

    let new_id = edited_profile_id(&temporary).map_err(|error| {
        anyhow::anyhow!(
            "Configuration was not saved: {error}\nEdited copy kept at: {}",
            temporary.display(),
        )
    })?;
    replace_profile_with_edited_copy(&profile, &temporary, id, &new_id)?;
    println!("Saved profile: {new_id}");
    Ok(())
}

/// Validates just the durable document identity. Runtime availability is checked
/// later by the runtime selected in the profile, not while an editor is saving.
fn edited_profile_id(temporary: &Path) -> Result<String> {
    let text = fs::read_to_string(temporary)
        .with_context(|| format!("read edited profile {}", temporary.display()))?;
    let document: toml::Value = text.parse().context("invalid TOML")?;
    let id = document
        .get("id")
        .and_then(toml::Value::as_str)
        .context("top-level id must be a string")?
        .to_owned();
    validate_profile_id(&id)?;
    Ok(id)
}

fn replace_profile_with_edited_copy(
    original: &Path,
    temporary: &Path,
    original_id: &str,
    new_id: &str,
) -> Result<()> {
    let destination = original.with_file_name(format!("{new_id}.toml"));
    if destination == original {
        fs::rename(temporary, original).with_context(|| format!("save profile {new_id}"))?;
        return Ok(());
    }
    ensure!(
        !destination.exists(),
        "profile id '{new_id}' already exists"
    );

    let backup = original.with_file_name(format!(".{original_id}.rename-backup.tmp"));
    ensure!(
        !backup.exists(),
        "a previous profile rename backup exists: {}; inspect or remove it before retrying",
        backup.display(),
    );
    fs::rename(original, &backup)
        .with_context(|| format!("prepare rename of profile {original_id}"))?;
    if let Err(error) = fs::rename(temporary, &destination) {
        let restored = fs::rename(&backup, original);
        return match restored {
            Ok(()) => Err(error).context("save edited profile; original profile was restored"),
            Err(restore_error) => Err(anyhow::anyhow!(
                "save edited profile failed: {error}; original profile backup remains at {}: {restore_error}",
                backup.display(),
            )),
        };
    }
    if let Err(error) = fs::remove_file(&backup) {
        eprintln!(
            "Saved profile '{new_id}', but could not remove temporary backup {}: {error}",
            backup.display(),
        );
    }
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
