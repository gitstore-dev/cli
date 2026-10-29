//! HTTP-only adapter for libgit2's maintained smart protocol implementation.
//! libgit2 owns pack negotiation/push; reqwest enforces absolute deadlines and
//! rejects *all* redirects (libgit2's built-in policy permits same-host redirects).
use super::Auth;
use crate::error::{Error, Result};
use git2::transport::{self, Service, SmartSubtransport, SmartSubtransportStream};
use std::{
    io::{self, Read, Seek, SeekFrom, Write},
    sync::{Arc, Mutex, MutexGuard, OnceLock, Weak},
    time::{Duration, Instant},
};
static ACTIVE: Mutex<()> = Mutex::new(());
static CONTEXT: OnceLock<Mutex<Weak<Context>>> = OnceLock::new();
static REGISTERED: OnceLock<Result<()>> = OnceLock::new();
struct Context {
    client: reqwest::blocking::Client,
    auth: Arc<Auth>,
    allowed: Vec<String>,
    deadline: Instant,
    error: Mutex<Option<Error>>,
}
pub(super) struct Network {
    context: Arc<Context>,
    _exclusive: MutexGuard<'static, ()>,
}
impl Network {
    pub fn install(auth: Arc<Auth>, allowed: Vec<String>, deadline: Instant) -> Result<Self> {
        let exclusive = ACTIVE
            .lock()
            .map_err(|_| Error::remote("Git transport lock unavailable"))?;
        let context = Arc::new(Context {
            client: reqwest::blocking::Client::builder()
                .redirect(reqwest::redirect::Policy::none())
                .connect_timeout(Duration::from_secs(5))
                .build()
                .map_err(|_| Error::remote("Cannot initialize Git HTTP client"))?,
            auth,
            allowed,
            deadline,
            error: Mutex::new(None),
        });
        *CONTEXT
            .get_or_init(|| Mutex::new(Weak::new()))
            .lock()
            .map_err(|_| Error::remote("Git transport context unavailable"))? =
            Arc::downgrade(&context);
        REGISTERED
            .get_or_init(|| {
                for prefix in ["http", "https"] {
                    // All gs Git operations hold ACTIVE for their complete lifetime. No other
                    // transport is created while registering these factories. Factories retain
                    // a Weak context, so they cannot keep credentials alive after the operation.
                    unsafe {
                        transport::register(prefix, |remote| {
                            let context = CONTEXT
                                .get()
                                .and_then(|m| m.lock().ok()?.upgrade())
                                .ok_or_else(|| {
                                    git2::Error::from_str("Git HTTP context unavailable")
                                })?;
                            transport::Transport::smart(remote, true, Http { context })
                        })
                    }
                    .map_err(|_| Error::remote("Cannot register bounded Git HTTP adapter"))?;
                }
                Ok(())
            })
            .clone()?;
        Ok(Self {
            context,
            _exclusive: exclusive,
        })
    }
    pub fn error(&self) -> Option<Error> {
        self.context.error.lock().ok()?.take()
    }
}
struct Http {
    context: Arc<Context>,
}
impl SmartSubtransport for Http {
    fn action(
        &self,
        url: &str,
        service: Service,
    ) -> std::result::Result<Box<dyn SmartSubtransportStream>, git2::Error> {
        if !self.context.allowed.iter().any(|s| s == url) {
            return Err(git2::Error::from_str(
                "Git HTTP URL outside selected repository",
            ));
        }
        let (operation, listing) = match service {
            Service::UploadPackLs => ("git-upload-pack", true),
            Service::UploadPack => ("git-upload-pack", false),
            Service::ReceivePackLs => ("git-receive-pack", true),
            Service::ReceivePack => ("git-receive-pack", false),
        };
        let url = if listing {
            format!("{url}/info/refs?service={operation}")
        } else {
            format!("{url}/{operation}")
        };
        let body =
            tempfile::tempfile().map_err(|_| git2::Error::from_str("Cannot spool Git request"))?;
        Ok(Box::new(Stream {
            context: self.context.clone(),
            url,
            operation,
            listing,
            body,
            response: None,
        }))
    }
    fn close(&self) -> std::result::Result<(), git2::Error> {
        Ok(())
    }
}
struct Stream {
    context: Arc<Context>,
    url: String,
    operation: &'static str,
    listing: bool,
    body: std::fs::File,
    response: Option<reqwest::blocking::Response>,
}
impl Stream {
    fn fail(&self, error: Error) -> io::Error {
        if let Ok(mut slot) = self.context.error.lock() {
            *slot = Some(error)
        }
        io::Error::other("Git HTTP request failed")
    }
    fn remaining(&self) -> io::Result<Duration> {
        self.context
            .deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(|| self.fail(Error::deadline()))
    }
    fn begin(&mut self) -> io::Result<()> {
        let remaining = self.remaining()?;
        let client = &self.context.client;
        let mut request = if self.listing {
            client.get(&self.url)
        } else {
            self.body.seek(SeekFrom::Start(0))?;
            let file = self.body.try_clone()?;
            let len = file.metadata()?.len();
            client
                .post(&self.url)
                .header(
                    "Content-Type",
                    format!("application/x-{}-request", self.operation),
                )
                .body(reqwest::blocking::Body::sized(file, len))
        }
        .timeout(remaining)
        .header(
            "Accept",
            format!(
                "application/x-{}-{}",
                self.operation,
                if self.listing {
                    "advertisement"
                } else {
                    "result"
                }
            ),
        );
        request = match self.context.auth.as_ref() {
            Auth::Basic { username, password } => {
                request.basic_auth(username, Some(password.expose()))
            }
            Auth::Bearer { token } => request.bearer_auth(token.expose()),
        };
        let response = request.send().map_err(|e| {
            self.fail(if e.is_timeout() {
                Error::deadline()
            } else {
                Error::remote("Git HTTP connection/response failed")
            })
        })?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(self.fail(match status {
                401 | 403 | 407 => Error::auth("Git authentication or authorization failed"),
                404 => Error::new(4, "NOT_FOUND", "Git target not found"),
                _ => Error::remote("Git HTTP failed or attempted a redirect"),
            }));
        }
        let expected = format!(
            "application/x-{}-{}",
            self.operation,
            if self.listing {
                "advertisement"
            } else {
                "result"
            }
        );
        if response
            .headers()
            .get("Content-Type")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.split(';').next())
            != Some(expected.as_str())
        {
            return Err(self.fail(Error::remote("Invalid Git HTTP response content type")));
        }
        self.response = Some(response);
        Ok(())
    }
}
impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.remaining()?;
        if self.response.is_none() {
            self.begin()?
        }
        self.response
            .as_mut()
            .expect("response initialized")
            .read(buf)
            .map_err(|_| {
                self.fail(if Instant::now() >= self.context.deadline {
                    Error::deadline()
                } else {
                    Error::remote("Git HTTP response interrupted")
                })
            })
    }
}
impl Write for Stream {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.remaining()?;
        self.body.write(data)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.body.flush()
    }
}
