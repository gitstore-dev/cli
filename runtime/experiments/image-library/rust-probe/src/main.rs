fn main() {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 4 || args[1] != "fetch-fixture" {
        eprintln!("usage: image probe fetch-fixture LOOPBACK_DIGEST_REFERENCE NEW_OWNED_OUTPUT");
        std::process::exit(2);
    }
    if let Err(error) =
        gitstore_image_library_probe::fetch_fixture(&args[2], std::path::Path::new(&args[3]))
    {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
