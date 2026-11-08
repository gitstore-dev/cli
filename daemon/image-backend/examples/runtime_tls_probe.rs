//! Contributor-only, network-free TLS builder diagnostics in a selected fixture.
//! Never linked into gitstored or installed with a candidate package.
#[cfg(target_os = "linux")]
fn main() {
    use std::os::unix::fs::MetadataExt;
    use std::path::Path;
    let arguments: Vec<_> = std::env::args().collect();
    assert!(
        arguments.len() == 3 && arguments[1] == "--fixture",
        "select --fixture ABS"
    );
    let path = Path::new(&arguments[2]);
    let info = path.symlink_metadata().expect("selected fixture metadata");
    let uid = unsafe { libc::geteuid() };
    assert!(
        uid != 0
            && path.is_absolute()
            && info.is_file()
            && info.uid() == uid
            && info.nlink() == 1
            && info.mode() & 0o077 == 0
            && info.len() <= 65536,
        "select an owned bounded private fixture as a non-root account"
    );
    let value: serde_json::Value =
        serde_json::from_slice(&std::fs::read(path).expect("selected fixture"))
            .expect("fixture JSON");
    assert!(
        value["dedicatedTestAccount"] == true && value["disposableUid"] == uid,
        "select this dedicated disposable account"
    );
    for (kind, requested) in [
        (libc::RLIMIT_CORE, 0),
        (libc::RLIMIT_AS, 1024 * 1024 * 1024),
        (libc::RLIMIT_FSIZE, 8 * 1024 * 1024 * 1024),
        (libc::RLIMIT_NOFILE, 128),
    ] {
        let mut limit = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        assert_eq!(unsafe { libc::getrlimit(kind, &mut limit) }, 0);
        let maximum = limit.rlim_cur.min(requested);
        limit = libc::rlimit {
            rlim_cur: maximum,
            rlim_max: maximum,
        };
        assert_eq!(unsafe { libc::setrlimit(kind, &limit) }, 0);
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("bounded diagnostic Tokio runtime");
    runtime.block_on(async {
        // No URL, request, authentication, token or credential is ever configured.
        // A builder error therefore contains only system TLS initialization data.
        let result = reqwest::Client::builder()
            .tls_backend_native()
            .no_proxy()
            .build();
        match result {
            Ok(_) => println!("Bounded native TLS builder passed; no network request performed."),
            Err(error) => {
                eprintln!("Bounded native TLS builder failed: {error:?}");
                std::process::exit(1);
            }
        }
    });
}
#[cfg(not(target_os = "linux"))]
fn main() {
    eprintln!("Select a dedicated non-root Linux diagnostic fixture.");
    std::process::exit(2);
}
