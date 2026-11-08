//! Opt-in acceptance target; scripts/test-integration.sh owns its disposable stack.
use git2::{Oid, Repository};
use serde_json::Value;
use std::{env, os::unix::fs::PermissionsExt, path::Path, process::Command};

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

struct Fixture {
    temp: tempfile::TempDir,
    config: String,
    credential: String,
    name: String,
}
impl Fixture {
    fn new(name: &str) -> Self {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let mut fixture = Self {
            temp: tempfile::tempdir().unwrap(),
            config: env::var("GITSTORE_IMAGE_CONFIG").unwrap(),
            credential: env::var("GITSTORE_IMAGE_CREDENTIAL").unwrap(),
            name: format!(
                "{}-{:x}",
                name.replace('_', "-"),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ),
        };
        std::fs::set_permissions(fixture.temp.path(), std::fs::Permissions::from_mode(0o700))
            .unwrap();
        let config = fixture.temp.path().join("config.toml");
        std::fs::write(&config, std::fs::read(&fixture.config).unwrap()).unwrap();
        fixture.config = config.to_str().unwrap().to_owned();
        // Each test owns a distinct remote repository; no test-order dependency.
        let input = serde_json::json!({
            "apiVersion":"gitstore.dev/v1beta1", "kind":"Repository",
            "metadata":{"namespace":"gs-m1-ci", "name":fixture.name},
            "spec":{"defaultBranch":"main", "visibility":"PRIVATE", "storageClass":"standard"}
        });
        let envelope = fixture.temp.path().join("create-repository.json");
        std::fs::write(&envelope, serde_json::to_vec(&serde_json::json!({
            "query":"mutation($input:CreateRepositoryInput!){createRepository(input:$input){repository{id}}}",
            "variables":{"input":input}
        })).unwrap()).unwrap();
        fixture.cli(
            &["api", "graphql", "--input", envelope.to_str().unwrap()],
            fixture.temp.path(),
            0,
        );
        let base = env::var("GITSTORE_IMAGE_GIT_URL").unwrap();
        let url = format!("{}/{}.git", base.rsplit_once('/').unwrap().0, fixture.name);
        let token = std::fs::read_to_string(&fixture.credential).unwrap();
        let client = reqwest::blocking::Client::builder()
            .timeout(std::time::Duration::from_secs(10))
            .build()
            .unwrap();
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(60);
        loop {
            let response = client
                .get(format!("{url}/info/refs?service=git-upload-pack"))
                .bearer_auth(token.trim())
                .send()
                .unwrap();
            if response.status().is_success() {
                break;
            }
            assert!(
                std::time::Instant::now() < deadline,
                "Repository provisioning timed out"
            );
            std::thread::sleep(std::time::Duration::from_millis(200));
        }
        let mut init = git2::RepositoryInitOptions::new();
        init.initial_head("main").external_template(false);
        let seed = Repository::init_opts(fixture.temp.path().join("seed"), &init).unwrap();
        commit(&seed, "initial fixture\n");
        let mut remote = seed.remote_anonymous(&url).unwrap();
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
        fixture
    }
    fn cli(&self, args: &[&str], cwd: &Path, code: i32) -> Value {
        let output = Command::new(env!("CARGO_BIN_EXE_gs"))
            .env_clear()
            .env("PATH", "/nonexistent")
            .env("HOME", self.temp.path())
            .args([
                "--config",
                &self.config,
                "--credential-file",
                &self.credential,
                "--non-interactive",
                "-o",
                "json",
            ])
            .args(args)
            .current_dir(cwd)
            .output()
            .unwrap();
        assert_eq!(
            output.status.code(),
            Some(code),
            "CLI failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        if output.stdout.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(&output.stdout).unwrap()
        }
    }
    fn clone_to(&self, name: &str, extra: &[&str]) -> Repository {
        let path = self.temp.path().join(name);
        let mut args = vec!["repository", "clone", &self.name, path.to_str().unwrap()];
        args.extend_from_slice(extra);
        self.cli(&args, self.temp.path(), 0);
        Repository::open(path).unwrap()
    }
    fn sync(&self, repo: &Repository, code: i32) -> Value {
        self.cli(
            &["repository", "sync", &self.name],
            repo.workdir().unwrap(),
            code,
        )
    }
}

#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn file_admitted_over_public_git_hydrates_by_name_and_id_and_emits_typed_event() {
    let fixture = Fixture::new("file_projection");
    let repo = fixture.clone_to("file-authoring", &[]);
    let name = &fixture.name;
    let checkpoint = fixture.temp.path().join("file-watch.json");
    let mut observer = Command::new(env!("CARGO_BIN_EXE_gs"))
        .env_clear()
        .env("HOME", fixture.temp.path())
        .env("PATH", "/nonexistent")
        .args([
            "--config",
            &fixture.config,
            "--credential-file",
            &fixture.credential,
            "--non-interactive",
            "--timeout",
            "20s",
            "-o",
            "json",
            "watch",
            "file",
            name,
            "--checkpoint-file",
            checkpoint.to_str().unwrap(),
            "--max-events",
            "1",
        ])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while !checkpoint.exists() {
        assert!(
            observer.try_wait().unwrap().is_none(),
            "File watch closed before bootstrap"
        );
        assert!(
            std::time::Instant::now() < deadline,
            "File bootstrap timed out"
        );
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let path = format!("files/{name}.md");
    std::fs::create_dir(repo.workdir().unwrap().join("files")).unwrap();
    std::fs::write(repo.workdir().unwrap().join(&path), format!(
        "---\napiVersion: storage.gitstore.dev/v1beta1\nkind: File\nmetadata:\n  namespace: gs-m1-ci\n  name: {name}\nspec:\n  contentType: image/jpeg\n  provider: git\n  source:\n    uri: media/fixture.jpg\n---\nCafé\n\n"
    )).unwrap();
    let mut index = repo.index().unwrap();
    index.add_path(Path::new(&path)).unwrap();
    index.write().unwrap();
    commit(&repo, "Admit File through public Git\n");
    fixture.sync(&repo, 0);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(25);
    while observer.try_wait().unwrap().is_none() {
        if std::time::Instant::now() >= deadline {
            observer.kill().unwrap();
            observer.wait().unwrap();
            panic!("File event timed out");
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let output = observer.wait_with_output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let events: Vec<Value> = String::from_utf8(output.stdout)
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert_eq!(events[0]["type"], "BOOKMARK");
    let event = events
        .iter()
        .find(|event| event["type"] == "ADDED")
        .unwrap();
    assert_eq!(event["object"]["kind"], "File");
    let by_name = fixture.cli(&["read", "file", name], fixture.temp.path(), 0);
    let config = std::fs::read_to_string(&fixture.config).unwrap();
    std::fs::write(
        &fixture.config,
        config.replace("namespace = \"gs-m1-ci\"\n", ""),
    )
    .unwrap();
    let by_id = fixture.cli(
        &["read", "file", "--id", by_name["id"].as_str().unwrap()],
        fixture.temp.path(),
        0,
    );
    assert_eq!(by_name["id"], by_id["id"]);
    assert_eq!(by_id["body"], "Café\n\n");
    assert_eq!(by_id["spec"]["source"]["uri"], "media/fixture.jpg");
    assert_eq!(event["object"]["id"], by_id["id"]);
    assert!(by_id.get("status").is_some());
}

#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn clone_default_remote_and_branch_tracking() {
    let fixture = Fixture::new("clone_default");
    let repo = fixture.clone_to("clone", &[]);
    let remotes = repo.remotes().unwrap();
    assert_eq!(remotes.len(), 1);
    assert_eq!(remotes.get(0).unwrap(), Some("gitstore"));
    assert_eq!(
        repo.find_branch("main", git2::BranchType::Local)
            .unwrap()
            .upstream()
            .unwrap()
            .name()
            .unwrap(),
        Some("gitstore/main")
    );
}
#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn clone_explicit_remote_override() {
    let fixture = Fixture::new("clone_override");
    let repo = fixture.clone_to("clone", &["--remote", "origin"]);
    assert!(repo.find_remote("origin").is_ok());
    assert!(repo.find_remote("gitstore").is_err());
}
#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn sync_without_new_commit_is_not_submitted() {
    let fixture = Fixture::new("sync_noop");
    let repo = fixture.clone_to("clone", &[]);
    assert_eq!(fixture.sync(&repo, 0)["submission"], "not-submitted");
}
#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn sync_push_is_accepted_and_reclone_observes_revision() {
    let fixture = Fixture::new("sync_push");
    let repo = fixture.clone_to("clone", &[]);
    let revision = commit(&repo, "updated by acceptance\n");
    let result = fixture.sync(&repo, 0);
    assert_eq!(result["submission"], "accepted");
    assert_eq!(result["revision"], revision.to_string());
    assert_eq!(result["admission"], "unverified");
    assert_eq!(result["readiness"], "unverified");
    let verify = fixture.clone_to("verify", &[]);
    assert_eq!(verify.head().unwrap().target(), Some(revision));
}
#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn sync_dirty_worktree_preserves_local_changes() {
    let fixture = Fixture::new("sync_dirty");
    let repo = fixture.clone_to("clone", &[]);
    let original = repo.head().unwrap().target();
    let path = repo.workdir().unwrap().join("ci-marker.txt");
    std::fs::write(&path, "uncommitted\n").unwrap();
    fixture.sync(&repo, 2);
    assert_eq!(repo.head().unwrap().target(), original);
    assert_eq!(std::fs::read_to_string(path).unwrap(), "uncommitted\n");
}
#[test]
#[ignore = "Requires scripts/test-integration.sh"]
fn sync_divergent_history_preserves_local_commit() {
    let fixture = Fixture::new("sync_divergent");
    let first = fixture.clone_to("first", &[]);
    let second = fixture.clone_to("second", &[]);
    commit(&first, "remote advance\n");
    fixture.sync(&first, 0);
    let local = commit(&second, "independent local advance\n");
    fixture.sync(&second, 5);
    assert_eq!(second.head().unwrap().target(), Some(local));
    let verify = fixture.clone_to("verify", &[]);
    assert_eq!(
        verify.head().unwrap().target(),
        first.head().unwrap().target()
    );
}
