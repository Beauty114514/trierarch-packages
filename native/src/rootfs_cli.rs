use std::env;
use std::path::PathBuf;

fn main() {
    if let Err(error) = run(env::args().skip(1).collect()) {
        eprintln!("trierarch-rootfs: {error:#}");
        eprintln!("usage: trierarch-rootfs import <archive.tar.xz> --name <rootfs-name>");
        std::process::exit(1);
    }
}

fn run(arguments: Vec<String>) -> anyhow::Result<()> {
    let [command, archive, flag, name] = arguments.as_slice() else {
        anyhow::bail!("invalid arguments");
    };
    anyhow::ensure!(command == "import", "unknown command: {command}");
    anyhow::ensure!(flag == "--name", "expected --name <rootfs-name>");
    let files_directory = env::var_os("TRIERARCH_FILES_DIR")
        .map(PathBuf::from)
        .ok_or_else(|| anyhow::anyhow!("TRIERARCH_FILES_DIR is not set"))?;
    let archive = PathBuf::from(archive);
    let archive = if archive.is_absolute() {
        archive
    } else {
        env::current_dir()?.join(archive)
    };
    let installed = trierarch_native::rootfs::import_xz_tar(&archive, &files_directory, name)?;
    println!("Imported rootfs: {}", installed.display());
    Ok(())
}
