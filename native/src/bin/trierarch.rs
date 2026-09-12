use std::env;

fn main() {
    if let Err(error) = trierarch_native::cli::run(env::args_os().skip(1).collect()) {
        eprintln!("trierarch: {error:#}");
        std::process::exit(1);
    }
}
