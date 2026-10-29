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

struct Fixture {
    temp: tempfile::TempDir,
    config: String,
    credential: String,
    name: String,
}
impl Fixture {
    fn new(name: &str) -> Self {
        let fixture = Self {
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
