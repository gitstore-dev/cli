//! Minimal contract fixture for upload-pack/receive-pack, backed by libgit2.
//! This is test-only protocol orchestration, not production server functionality.
use git2::{Oid, Repository};
use std::{
    io::Write,
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::Duration,
};
use tiny_http::{Header, Request, Response, Server};
#[derive(Clone)]
pub enum Mode {
    Normal,
    Deny(u16),
    Redirect,
    Delay,
    LostResponse,
}
#[derive(Clone)]
pub enum Auth {
    Basic,
    Bearer,
    None,
}
pub struct HttpGit {
    pub endpoint: String,
    pub path: PathBuf,
    pub calls: Arc<Mutex<Vec<String>>>,
    pub reject_push: Arc<AtomicBool>,
    pub advance: Arc<Mutex<Option<Oid>>>,
    pub mode: Arc<Mutex<Mode>>,
    stop: Arc<AtomicBool>,
    thread: Option<thread::JoinHandle<()>>,
}
impl HttpGit {
    pub fn new(path: &Path, auth: Auth) -> Self {
        let server = Server::http("127.0.0.1:0").unwrap();
        let endpoint = format!("http://{}/prefix", server.server_addr());
        let stop = Arc::new(AtomicBool::new(false));
        let calls = Arc::new(Mutex::new(vec![]));
        let reject = Arc::new(AtomicBool::new(false));
        let advance = Arc::new(Mutex::new(None));
        let mode = Arc::new(Mutex::new(Mode::Normal));
        let request_mode = mode.clone();
        let (s, c, r, a, p) = (
            stop.clone(),
            calls.clone(),
            reject.clone(),
            advance.clone(),
            path.to_owned(),
        );
        let handle = thread::spawn(move || {
            while !s.load(Ordering::Relaxed) {
                if let Some(request) = server.recv_timeout(Duration::from_millis(50)).unwrap() {
                    let route = request.url().to_owned();
                    c.lock().unwrap().push(route);
                    handle(
                        request,
                        &p,
                        &auth,
                        r.load(Ordering::Relaxed),
                        &a,
                        request_mode.lock().unwrap().clone(),
                    );
                }
            }
        });
        Self {
            endpoint,
            path: path.into(),
            calls,
            reject_push: reject,
            advance,
            mode,
            stop,
            thread: Some(handle),
        }
    }
}
impl Drop for HttpGit {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            t.join().unwrap()
        }
    }
}
fn pkt(b: &[u8]) -> Vec<u8> {
    let mut v = format!("{:04x}", b.len() + 4).into_bytes();
    v.extend_from_slice(b);
    v
}
fn header(k: &str, v: &str) -> Header {
    Header::from_bytes(k, v).unwrap()
}
fn send(r: Request, code: u16, body: Vec<u8>, kind: &str) {
    let _ = r.respond(
        Response::from_data(body)
            .with_status_code(code)
            .with_header(header("Content-Type", kind)),
    );
}
fn handle(
    mut r: Request,
    path: &Path,
    auth: &Auth,
    reject: bool,
    advance: &Arc<Mutex<Option<Oid>>>,
    mode: Mode,
) {
    match mode {
        Mode::Deny(code) => {
            send(r, code, vec![], "text/plain");
            return;
        }
        Mode::Redirect => {
            let _ = r.respond(
                Response::from_string("")
                    .with_status_code(302)
                    .with_header(header("Location", "/outside")),
            );
            return;
        }
        Mode::Delay => std::thread::sleep(Duration::from_millis(300)),
        _ => {}
    }
    let got = r
        .headers()
        .iter()
        .find(|h| h.field.equiv("Authorization"))
        .map(|h| h.value.as_str())
        .unwrap_or("");
    let valid = match auth {
        Auth::None => true,
        Auth::Bearer => got == "Bearer synthetic-git-token",
        Auth::Basic => got == "Basic dXNlcjpwYXNzd29yZA==",
    };
    if !valid {
        let _ = r.respond(
            Response::from_string("")
                .with_status_code(401)
                .with_header(header("WWW-Authenticate", "Basic realm=fixture")),
        );
        return;
    }
    let url = r.url().to_string();
    let repo = Repository::open_bare(path).unwrap();
    if url.starts_with("/prefix/shop/catalog.git/info/refs?") {
        let receive = url.ends_with("git-receive-pack");
        let service = if receive {
            "git-receive-pack"
        } else {
            "git-upload-pack"
        };
        if receive && let Some(id) = advance.lock().unwrap().take() {
            repo.reference("refs/heads/main", id, true, "fixture race")
                .unwrap();
        }
        let mut bytes = pkt(format!("# service={service}\n").as_bytes());
        bytes.extend_from_slice(b"0000");
        let refs: Vec<_> = repo
            .references()
            .unwrap()
            .filter_map(|r| r.ok())
            .filter(|r| r.is_branch())
            .collect();
        let caps = if receive {
            "report-status ofs-delta"
        } else {
            "multi_ack_detailed side-band-64k ofs-delta no-progress symref=HEAD:refs/heads/main"
        };
        if refs.is_empty() {
            bytes.extend(pkt(format!(
                "{} capabilities^{{}}\0{caps}\n",
                Oid::ZERO_SHA1
            )
            .as_bytes()));
        } else {
            if !receive {
                let head = repo.head().unwrap().target().unwrap();
                bytes.extend(pkt(format!("{head} HEAD\0{caps}\n").as_bytes()));
            }
            for (i, reference) in refs.iter().enumerate() {
                let name = reference.name().unwrap();
                let id = reference.target().unwrap();
                bytes.extend(pkt(if receive && i == 0 {
                    format!("{id} {name}\0{caps}\n")
                } else {
                    format!("{id} {name}\n")
                }
                .as_bytes()));
            }
        }
        bytes.extend_from_slice(b"0000");
        send(
            r,
            200,
            bytes,
            &format!("application/x-{service}-advertisement"),
        );
        return;
    }
    let mut body = Vec::new();
    r.as_reader().read_to_end(&mut body).unwrap();
    if url == "/prefix/shop/catalog.git/git-upload-pack" {
        let mut walk = repo.revwalk().unwrap();
        walk.push_head().unwrap();
        let mut pack = repo.packbuilder().unwrap();
        pack.insert_walk(&mut walk).unwrap();
        let mut buf = git2::Buf::new();
        pack.write_buf(&mut buf).unwrap();
        let mut bytes = pkt(b"NAK\n");
        for chunk in buf.chunks(65000) {
            let mut band = vec![1];
            band.extend_from_slice(chunk);
            bytes.extend(pkt(&band));
        }
        bytes.extend_from_slice(b"0000");
        send(r, 200, bytes, "application/x-git-upload-pack-result");
        return;
    }
    if url == "/prefix/shop/catalog.git/git-receive-pack" {
        let size = usize::from_str_radix(std::str::from_utf8(&body[..4]).unwrap(), 16).unwrap();
        let command = std::str::from_utf8(&body[4..size])
            .unwrap()
            .split('\0')
            .next()
            .unwrap();
        let fields: Vec<_> = command.split_whitespace().collect();
        let old = Oid::from_str(fields[0]).unwrap();
        let new = Oid::from_str(fields[1]).unwrap();
        let name = fields[2];
        let pack_offset = body.windows(4).position(|x| x == b"PACK");
        if let Some(offset) = pack_offset {
            let odb = repo.odb().unwrap();
            let mut writer = odb.packwriter().unwrap();
            writer.write_all(&body[offset..]).unwrap();
            writer.commit().unwrap();
        }
        let actual = repo
            .find_reference(name)
            .ok()
            .and_then(|r| r.target())
            .unwrap_or(Oid::ZERO_SHA1);
        let refused = reject || actual != old;
        if !refused {
            repo.reference(name, new, true, "fixture receive").unwrap();
        }
        if matches!(mode, Mode::LostResponse) {
            drop(r);
            return;
        }
        let mut response = pkt(b"unpack ok\n");
        response.extend(pkt(if refused {
            format!("ng {name} fixture rejection\n")
        } else {
            format!("ok {name}\n")
        }
        .as_bytes()));
        response.extend_from_slice(b"0000");
        send(r, 200, response, "application/x-git-receive-pack-result");
        return;
    }
    send(r, 501, b"outside fixture".to_vec(), "text/plain");
}
pub fn commit(repo: &Repository, text: &str) -> Oid {
    let dir = repo.workdir().unwrap();
    std::fs::write(dir.join("body.md"), text).unwrap();
    let mut index = repo.index().unwrap();
    index.add_path(Path::new("body.md")).unwrap();
    index.write().unwrap();
    let tree = repo.find_tree(index.write_tree().unwrap()).unwrap();
    let sig = git2::Signature::now("Fixture", "fixture@example.test").unwrap();
    let parent = repo.head().ok().and_then(|r| r.peel_to_commit().ok());
    repo.commit(
        Some("HEAD"),
        &sig,
        &sig,
        text,
        &tree,
        &parent.iter().collect::<Vec<_>>(),
    )
    .unwrap()
}
pub fn seed(path: &Path) -> Repository {
    let mut opts = git2::RepositoryInitOptions::new();
    opts.initial_head("main").external_template(false);
    Repository::init_opts(path, &opts).unwrap()
}
