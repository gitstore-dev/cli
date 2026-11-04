//! Developer fixture only; never installed or used by gs/gitstored.
#[cfg(unix)]
use git2::{Repository, Signature};
#[cfg(unix)]
use std::{fs, os::unix::fs::MetadataExt, path::Path};

#[cfg(unix)]
fn run() -> Result<(), Box<dyn std::error::Error>> {
    const MARKER: &[u8] = b"gitstore-disposable-v1\n";
    let args: Vec<_> = std::env::args().collect();
    let retained = args.len() == 4 && args[1] == "retained";
    if !retained
        && (args.len() != 3
            || !matches!(
                args[1].as_str(),
                "seed" | "head" | "advance" | "seed-remote"
            ))
    {
        return Err("Select seed/head and an explicit disposable clone".into());
    }
    let path = Path::new(&args[2]);
    let uid = unsafe { libc::geteuid() };
    let parent = if retained {
        path
    } else {
        path.parent().ok_or("Missing fixture parent")?
    };
    let marker = parent.join(".gitstored-disposable-test");
    let identity = fs::symlink_metadata(&marker)?;
    let directory = fs::symlink_metadata(parent)?;
    if uid == 0
        || !path.is_absolute()
        || path.canonicalize()? != path
        || !directory.is_dir()
        || directory.uid() != uid
        || directory.mode() & 0o7777 != 0o700
        || !identity.is_file()
        || identity.uid() != uid
        || identity.nlink() != 1
        || identity.mode() & 0o7777 != 0o600
        || identity.len() != MARKER.len() as u64
        || fs::read(marker)? != MARKER
    {
        return Err("Select an owned marked disposable fixture".into());
    }
    if retained {
        // Test instrumentation only, after observed stop under the actual writer
        // lease. No product operation falls back to server repository files.
        let expected = git2::Oid::from_str(&args[3])?;
        let root = path.join("state/volumes");
        let device = fs::symlink_metadata(&root)?.dev();
        let mut pending = vec![(root, 0)];
        let mut visited = 0;
        while let Some((directory, depth)) = pending.pop() {
            visited += 1;
            if visited > 4096 || depth > 12 {
                return Err("Fixture traversal bound".into());
            }
            let identity = fs::symlink_metadata(&directory)?;
            if !identity.is_dir() || identity.uid() != uid || identity.dev() != device {
                return Err("Foreign fixture directory".into());
            }
            if let Ok(repository) = Repository::open_bare(&directory)
                && let Ok(commit) = repository.find_commit(expected)
            {
                let tree = commit.tree()?;
                let entry = tree.get_path(Path::new("candidate-marker.txt"))?;
                let blob = repository.find_blob(entry.id())?;
                if blob.content() != b"candidate advanced fixture\n" {
                    return Err("Retained fixture contents changed".into());
                }
                println!("{expected}");
                return Ok(());
            }
            for entry in fs::read_dir(&directory)? {
                let entry = entry?;
                if entry.file_type()?.is_symlink() {
                    return Err("Linked fixture input".into());
                }
                if entry.file_type()?.is_dir() {
                    pending.push((entry.path(), depth + 1));
                }
            }
        }
        return Err("Retained commit is missing".into());
    }
    let repo = Repository::open(path)?;
    if matches!(args[1].as_str(), "seed" | "advance") {
        if repo.is_empty()? != (args[1] == "seed") {
            return Err("Unexpected disposable history".into());
        }
        repo.set_head("refs/heads/main")?;
        fs::write(
            path.join("candidate-marker.txt"),
            if args[1] == "seed" {
                b"candidate retained fixture\n"
            } else {
                b"candidate advanced fixture\n"
            },
        )?;
        let mut index = repo.index()?;
        index.add_path(Path::new("candidate-marker.txt"))?;
        index.write()?;
        let tree = repo.find_tree(index.write_tree()?)?;
        let signature = Signature::now("Candidate fixture", "candidate@example.test")?;
        let parent = repo.head().ok().and_then(|head| head.peel_to_commit().ok());
        repo.commit(
            Some("HEAD"),
            &signature,
            &signature,
            "synthetic candidate fixture",
            &tree,
            &parent.iter().collect::<Vec<_>>(),
        )?;
    }
    if args[1] == "seed-remote" {
        let credentials_path = parent.join("public-git.json");
        let identity = fs::symlink_metadata(&credentials_path)?;
        if !identity.is_file()
            || identity.uid() != uid
            || identity.nlink() != 1
            || identity.mode() & 0o7777 != 0o600
            || identity.len() > 16384
        {
            return Err("Invalid synthetic credential source".into());
        }
        let value: serde_json::Value = serde_json::from_slice(&fs::read(credentials_path)?)?;
        let username = value["username"]
            .as_str()
            .ok_or("Missing synthetic username")?;
        let password = value["password"]
            .as_str()
            .ok_or("Missing synthetic password")?;
        let configured = repo.find_remote("gitstore")?;
        let target = url::Url::parse(configured.url()?)?;
        if target.scheme() != "http"
            || target.host_str() != Some("127.0.0.1")
            || target.port().is_none()
            || !target.username().is_empty()
            || target.password().is_some()
            || target.path() != "/candidate-native/catalog.git"
            || target.query().is_some()
            || target.fragment().is_some()
        {
            return Err("Remote is not the explicitly disposable local service".into());
        }
        let mut remote = repo.remote_anonymous(target.as_str())?;
        let mut callbacks = git2::RemoteCallbacks::new();
        callbacks.credentials(|_, _, _| git2::Cred::userpass_plaintext(username, password));
        callbacks.push_update_reference(|_, status| {
            if status.is_some() {
                Err(git2::Error::from_str("Synthetic seed rejected"))
            } else {
                Ok(())
            }
        });
        let mut options = git2::PushOptions::new();
        options
            .remote_callbacks(callbacks)
            .follow_redirects(git2::RemoteRedirect::None);
        remote.push(&["refs/heads/main:refs/heads/main"], Some(&mut options))?;
    }
    println!("{}", repo.head()?.peel_to_commit()?.id());
    Ok(())
}
#[cfg(unix)]
fn main() {
    if run().is_err() {
        eprintln!("Disposable repository fixture failed");
        std::process::exit(1);
    }
}
#[cfg(not(unix))]
fn main() {
    eprintln!("Select an explicitly disposable Unix fixture");
    std::process::exit(77);
}
