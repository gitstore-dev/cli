//! Opt-in acceptance target; scripts/test-integration.sh owns its disposable stack.
use git2::{Oid, Repository};
use serde_json::Value;
use std::{env, path::Path, process::Command};

fn commit(repo: &Repository, text: &str) -> Oid {
    std::fs::write(repo.workdir().unwrap().join("ci-marker.txt"), text).unwrap();
    let mut index = repo.index().unwrap();
    index.add_path(Path::new("ci-marker.txt")).unwrap();
    index.write().unwrap();
    let tree = repo.find_tree(index.write_tree().unwrap()).unwrap();
    let signature = git2::Signature::now("CI", "ci@example.test").unwrap();
    let parent = repo.head().ok().and_then(|head| head.peel_to_commit().ok());
    repo.commit(
        Some("HEAD"),
        &signature,
        &signature,
        text,
        &tree,
        &parent.iter().collect::<Vec<_>>(),
    )
    .unwrap()
}

#[test]
#[ignore = "Requires isolated published images; run scripts/test-integration.sh"]
fn published_images_clone_and_sync() {
    let temp = tempfile::tempdir().unwrap();
    let config = env::var("GITSTORE_IMAGE_CONFIG").unwrap();
    let credential = env::var("GITSTORE_IMAGE_CREDENTIAL").unwrap();
    let cli = |args: &[&str], cwd: &Path| -> Value {
        let output = Command::new(env!("CARGO_BIN_EXE_gs"))
            .env_clear()
            .env("PATH", "/nonexistent")
            .env("HOME", temp.path())
            .args([
                "--config",
                &config,
                "--credential-file",
                &credential,
                "--non-interactive",
                "-o",
                "json",
            ])
            .args(args)
            .current_dir(cwd)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "CLI failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        serde_json::from_slice(&output.stdout).unwrap()
    };
    // Seed through public Git with libgit2; no Git executable or server files.
    let seed_path = temp.path().join("seed");
    let mut init = git2::RepositoryInitOptions::new();
    init.initial_head("main").external_template(false);
    let seed = Repository::init_opts(&seed_path, &init).unwrap();
    commit(&seed, "initial fixture\n");
    let mut remote = seed
        .remote_anonymous(&env::var("GITSTORE_IMAGE_GIT_URL").unwrap())
        .unwrap();
    let token = std::fs::read_to_string(&credential).unwrap();
    let header = format!("Authorization: Bearer {}", token.trim());
    let mut callbacks = git2::RemoteCallbacks::new();
    callbacks.push_update_reference(|_, status| {
        assert!(status.is_none(), "Seed push rejected");
        Ok(())
    });
    let mut options = git2::PushOptions::new();
    options
        .custom_headers(&[&header])
        .remote_callbacks(callbacks);
    remote
        .push(&["refs/heads/main:refs/heads/main"], Some(&mut options))
        .unwrap();
    let clone_path = temp.path().join("clone");
    cli(
        &[
            "repository",
            "clone",
            "catalog",
            clone_path.to_str().unwrap(),
        ],
        temp.path(),
    );
    let repo = Repository::open(&clone_path).unwrap();
    assert_eq!(repo.remotes().unwrap().get(0).unwrap(), Some("gitstore"));
    assert_eq!(
        repo.find_branch("main", git2::BranchType::Local)
            .unwrap()
            .upstream()
            .unwrap()
            .name()
            .unwrap(),
        Some("gitstore/main")
    );
    assert_eq!(
        cli(&["repository", "sync", "catalog"], &clone_path)["submission"],
        "not-submitted"
    );
    let revision = commit(&repo, "updated by CLI acceptance\n");
    let result = cli(&["repository", "sync", "catalog"], &clone_path);
    assert_eq!(result["submission"], "accepted");
    assert_eq!(result["revision"], revision.to_string());
    assert_eq!(result["admission"], "unverified");
    assert_eq!(result["readiness"], "unverified");
    let verify = temp.path().join("verify");
    cli(
        &[
            "repository",
            "clone",
            "catalog",
            verify.to_str().unwrap(),
            "--remote",
            "origin",
        ],
        temp.path(),
    );
    let repo = Repository::open(verify).unwrap();
    assert_eq!(repo.head().unwrap().target(), Some(revision));
    assert!(repo.find_remote("origin").is_ok());
}
