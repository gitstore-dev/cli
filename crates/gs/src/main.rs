fn main() {
    #[cfg(unix)]
    if let Some(status) = gs::runtime_credentials::entry() {
        std::process::exit(status);
    }
    std::process::exit(gs::run());
}
