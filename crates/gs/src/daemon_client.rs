//! Owner-authenticated, deadline-bounded Unix socket client. No runtime downloads.
use crate::daemon_protocol::{self as wire, Identifier, ProtocolError};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    fs,
    io::{self, Read, Write},
    os::unix::{
        fs::{FileTypeExt, MetadataExt},
        net::UnixStream,
    },
    path::{Component, Path, PathBuf},
    time::{Duration, Instant},
};

#[derive(Debug)]
pub struct ClientError {
    pub code: String,
    pub stage: String,
    pub message: String,
    pub exit: i32,
    pub retryable: bool,
    pub operation_id: Option<String>,
    pub deployment_id: Option<String>,
    pub idempotency_key: Option<String>,
}
impl std::fmt::Display for ClientError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}
impl std::error::Error for ClientError {}
fn failure(code: &str, stage: &str, message: &str, exit: i32) -> Box<ClientError> {
    Box::new(ClientError {
        code: code.into(),
        stage: stage.into(),
        message: message.into(),
        exit,
        retryable: false,
        operation_id: None,
        deployment_id: None,
        idempotency_key: None,
    })
}
fn transport(error: io::Error) -> Box<ClientError> {
    if matches!(
        error.kind(),
        io::ErrorKind::InvalidData | io::ErrorKind::InvalidInput
    ) {
        invalid()
    } else if matches!(
        error.kind(),
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
    ) {
        failure(
            "DEADLINE_EXCEEDED",
            "transport",
            "Local observation deadline exceeded",
            7,
        )
    } else {
        failure(
            "LOCAL_TRANSPORT",
            "transport",
            "Local manager connection failed; inspect or replay with the same key",
            8,
        )
    }
}
fn invalid() -> Box<ClientError> {
    failure(
        "INVALID_ARGUMENT",
        "parse",
        "Invalid local protocol response",
        2,
    )
}
fn timeout_valid(timeout: Duration) -> bool {
    timeout.as_millis() >= 1 && timeout <= Duration::from_secs(3600)
}
fn validate_socket(path: &Path) -> Result<(), Box<ClientError>> {
    if !path.is_absolute() {
        return Err(failure(
            "INVALID_ARGUMENT",
            "resolve",
            "Select an absolute local socket path",
            2,
        ));
    }
    let mut current = PathBuf::new();
    for part in path.components() {
        if !matches!(part, Component::RootDir | Component::Normal(_)) {
            return Err(invalid());
        }
        current.push(part.as_os_str());
        let metadata = fs::symlink_metadata(&current).map_err(|_| {
            failure(
                "NOT_FOUND",
                "resolve",
                "Local manager socket is unavailable",
                4,
            )
        })?;
        if metadata.file_type().is_symlink() {
            return Err(failure(
                "UNAUTHORIZED_PEER",
                "authorize",
                "Symlinked manager paths are unsafe",
                3,
            ));
        }
    }
    let socket = fs::symlink_metadata(path).map_err(|_| invalid())?;
    let parent = fs::symlink_metadata(path.parent().ok_or_else(invalid)?).map_err(|_| invalid())?;
    let uid = unsafe { libc::geteuid() };
    if !socket.file_type().is_socket()
        || socket.uid() != uid
        || socket.mode() & 0o777 != 0o600
        || !parent.is_dir()
        || parent.uid() != uid
        || parent.mode() & 0o777 != 0o700
    {
        return Err(failure(
            "UNAUTHORIZED_PEER",
            "authorize",
            "Manager directory/socket ownership or permissions are unsafe",
            3,
        ));
    }
    Ok(())
}
fn peer_allowed(stream: &UnixStream) -> bool {
    use std::os::fd::AsRawFd;
    #[cfg(target_os = "linux")]
    unsafe {
        let mut credentials: libc::ucred = std::mem::zeroed();
        let mut length = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            (&mut credentials as *mut libc::ucred).cast(),
            &mut length,
        ) == 0
            && credentials.uid == libc::geteuid()
    }
    #[cfg(target_os = "macos")]
    unsafe {
        let (mut uid, mut gid) = (0, 0);
        libc::getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) == 0 && uid == libc::geteuid()
    }
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    {
        let _ = stream;
        false
    }
}
struct DeadlineIo<'a> {
    stream: &'a mut UnixStream,
    deadline: Instant,
}
impl DeadlineIo<'_> {
    fn remaining(&self) -> io::Result<Duration> {
        self.deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or_else(|| io::Error::from(io::ErrorKind::TimedOut))
    }
}
impl DeadlineIo<'_> {
    fn wait(&self, events: libc::c_short) -> io::Result<()> {
        use std::os::fd::AsRawFd;
        loop {
            let remaining = self.remaining()?;
            let mut descriptor = libc::pollfd {
                fd: self.stream.as_raw_fd(),
                events,
                revents: 0,
            };
            let result = unsafe {
                libc::poll(
                    &mut descriptor,
                    1,
                    remaining.as_millis().max(1).min(i32::MAX as u128) as i32,
                )
            };
            if result > 0 {
                return Ok(());
            }
            if result == 0 {
                continue;
            }
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
    }
}
impl Read for DeadlineIo<'_> {
    fn read(&mut self, b: &mut [u8]) -> io::Result<usize> {
        if b.is_empty() {
            return Ok(0);
        }
        loop {
            self.wait(libc::POLLIN)?;
            match self.stream.read(b) {
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    continue;
                }
                result => return result,
            }
        }
    }
}
impl Write for DeadlineIo<'_> {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        if b.is_empty() {
            return Ok(0);
        }
        loop {
            self.wait(libc::POLLOUT)?;
            match self.stream.write(b) {
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) =>
                {
                    continue;
                }
                result => return result,
            }
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
struct Pending {
    method: String,
    deadline: Instant,
    key: Option<String>,
    result: Option<Result<Value, Box<ClientError>>>,
}
pub struct Client {
    stream: UnixStream,
    next: u64,
    pending: BTreeMap<String, Pending>,
    capabilities: BTreeSet<String>,
    events: VecDeque<(Value, usize)>,
    event_bytes: usize,
    subscriptions: BTreeMap<String, Option<u64>>,
    frame_max: usize,
    pending_limit: usize,
}
impl Client {
    pub fn connect(
        path: &Path,
        required: &[&str],
        timeout: Duration,
    ) -> Result<Self, Box<ClientError>> {
        if !timeout_valid(timeout) {
            return Err(failure(
                "INVALID_ARGUMENT",
                "validate",
                "Local timeout must be 1 ms through one hour",
                2,
            ));
        }
        validate_socket(path)?;
        // Unix connect is nonblocking so a full backlog cannot defeat the deadline.
        let stream = connect_bounded(path, timeout).map_err(transport)?;
        if !peer_allowed(&stream) {
            return Err(failure(
                "UNAUTHORIZED_PEER",
                "authorize",
                "Local manager peer UID does not match",
                3,
            ));
        }
        let mut client = Self {
            stream,
            next: 0,
            pending: BTreeMap::new(),
            capabilities: BTreeSet::new(),
            events: VecDeque::new(),
            event_bytes: 0,
            subscriptions: BTreeMap::new(),
            frame_max: wire::FRAME_MAX,
            pending_limit: 16,
        };
        let id = client.submit_unchecked(
            "system.negotiate",
            json!({"supported":[{"major":1,"minor":0}],"requiredCapabilities":required}),
            None,
            timeout,
        )?;
        let result = client.receive(&id)?;
        if result["selectedVersion"] != json!({"major":1,"minor":0}) {
            return Err(failure(
                "INCOMPATIBLE_VERSION",
                "negotiate",
                "No compatible local protocol version",
                6,
            ));
        }
        let limits = result["limits"].as_object().ok_or_else(invalid)?;
        for name in [
            "frameBytes",
            "nesting",
            "stringBytes",
            "collectionItems",
            "connections",
            "outstandingPerConnection",
            "subscriptionsPerConnection",
            "queuedEventsPerSubscription",
            "queuedBytesPerSubscription",
            "globalQueuedBytes",
            "logsPerDeploymentBytes",
            "logMaxAgeDays",
            "globalLogBytes",
            "workers",
            "mutationPerDeployment",
            "pendingJobs",
            "partialFrameMs",
            "idleMs",
            "stopMs",
            "shutdownMs",
            "operationMetadataBytes",
            "operationRecords",
            "terminalRetentionDays",
        ] {
            if limits
                .get(name)
                .and_then(Value::as_u64)
                .is_none_or(|n| n == 0)
            {
                return Err(invalid());
            }
        }
        client.frame_max =
            (limits["frameBytes"].as_u64().ok_or_else(invalid)? as usize).min(wire::FRAME_MAX);
        client.pending_limit = (limits["outstandingPerConnection"]
            .as_u64()
            .ok_or_else(invalid)? as usize)
            .min(16);
        let caps = result["capabilities"].as_array().ok_or_else(invalid)?;
        if caps.len() > 64 {
            return Err(invalid());
        }
        for cap in caps {
            let cap = cap.as_str().filter(|s| !s.is_empty()).ok_or_else(invalid)?;
            if !client.capabilities.insert(cap.into()) {
                return Err(invalid());
            }
        }
        if required
            .iter()
            .any(|cap| !client.capabilities.contains(*cap))
        {
            return Err(failure(
                "UNSUPPORTED_CAPABILITY",
                "negotiate",
                "Required local capability is unavailable",
                6,
            ));
        }
        Ok(client)
    }
    pub fn submit(
        &mut self,
        method: &str,
        params: Value,
        key: Option<&str>,
        timeout: Duration,
    ) -> Result<String, Box<ClientError>> {
        let capability = match method {
            "system.capabilities" => "system.read",
            "deployment.validate" => "deployment.validate",
            "deployment.start" => "deployment.start",
            "deployment.inspect" | "deployment.list" => "deployment.read",
            "deployment.stop" => "deployment.stop",
            "operation.inspect" | "operation.wait" => "operation.read",
            "operation.cancel" => "operation.cancel",
            "migration.start" => "migration.start",
            "stream.subscribe" | "stream.unsubscribe" => "stream.read",
            _ => return Err(invalid()),
        };
        if !self.capabilities.contains(capability) {
            return Err(failure(
                "UNSUPPORTED_CAPABILITY",
                "validate",
                "Required local capability is unavailable",
                6,
            ));
        }
        self.submit_unchecked(method, params, key, timeout)
    }
    fn submit_unchecked(
        &mut self,
        method: &str,
        params: Value,
        key: Option<&str>,
        timeout: Duration,
    ) -> Result<String, Box<ClientError>> {
        if !timeout_valid(timeout) || timeout.as_millis() < 1 {
            return Err(invalid());
        }
        if self.pending.len() >= self.pending_limit {
            return Err(failure(
                "RESOURCE_LIMIT",
                "queue",
                "Too many pending local requests",
                8,
            ));
        }
        self.next = self.next.checked_add(1).ok_or_else(invalid)?;
        let id = format!("r{}", self.next);
        let mut value = json!({"version":{"major":1,"minor":0},"requestId":id,"method":method,"params":params,"timeoutMs":timeout.as_millis() as u64});
        if let Some(key) = key {
            value["idempotencyKey"] = json!(key);
        }
        wire::request(&value).map_err(|_| invalid())?;
        if serde_json::to_vec(&value).map_err(|_| invalid())?.len() > self.frame_max {
            return Err(failure(
                "RESOURCE_LIMIT",
                "validate",
                "Request exceeds negotiated frame limit",
                8,
            ));
        }
        let deadline = Instant::now() + timeout;
        let result = wire::write_frame(
            &mut DeadlineIo {
                stream: &mut self.stream,
                deadline,
            },
            &value,
        );
        if let Err(e) = result {
            let mut error = transport(e);
            error.idempotency_key = key.map(str::to_owned);
            return Err(error);
        }
        self.pending.insert(
            id.clone(),
            Pending {
                method: method.into(),
                deadline,
                key: key.map(str::to_owned),
                result: None,
            },
        );
        Ok(id)
    }
    pub fn receive(&mut self, wanted: &str) -> Result<Value, Box<ClientError>> {
        let key = self.pending.get(wanted).and_then(|p| p.key.clone());
        self.receive_inner(wanted).map_err(|mut error| {
            if error.idempotency_key.is_none() {
                error.idempotency_key = key;
            }
            error
        })
    }
    fn receive_inner(&mut self, wanted: &str) -> Result<Value, Box<ClientError>> {
        loop {
            let pending = self.pending.get_mut(wanted).ok_or_else(invalid)?;
            if let Some(result) = pending.result.take() {
                self.pending.remove(wanted);
                return result;
            }
            let deadline = pending.deadline;
            let response = match wire::read_frame(&mut DeadlineIo {
                stream: &mut self.stream,
                deadline,
            }) {
                Ok(Some(value)) => value,
                Ok(None) => {
                    let mut error = transport(io::Error::from(io::ErrorKind::UnexpectedEof));
                    error.idempotency_key = pending.key.clone();
                    return Err(error);
                }
                Err(e) => {
                    let mut error = transport(e);
                    error.idempotency_key = pending.key.clone();
                    return Err(error);
                }
            };
            if response["version"] != json!({"major":1,"minor":0}) {
                return Err(invalid());
            }
            if let Some(subscription) = response.get("subscriptionId") {
                let subscription = subscription.as_str().ok_or_else(invalid)?;
                let previous = self
                    .subscriptions
                    .get_mut(subscription)
                    .ok_or_else(invalid)?;
                let sequence = response["sequence"].as_u64().ok_or_else(invalid)?;
                if response.get("requestId").is_some()
                    || response.get("result").is_some()
                    || response.get("error").is_some()
                    || response.get("operationId").is_some()
                        == response.get("deploymentId").is_some()
                    || !response["payload"].is_object()
                    || response["type"].as_str().is_none_or(str::is_empty)
                    || previous.is_some_and(|n| sequence <= n)
                {
                    return Err(invalid());
                }
                for name in ["deploymentId", "operationId"] {
                    if let Some(value) = response.get(name) {
                        Identifier::try_from(value.as_str().ok_or_else(invalid)?.to_owned())
                            .map_err(|_| invalid())?;
                    }
                }
                let bytes = serde_json::to_vec(&response).map_err(|_| invalid())?.len();
                if self.events.len() >= 256 || self.event_bytes + bytes > 4 * 1024 * 1024 {
                    return Err(failure(
                        "STREAM_OVERFLOW",
                        "stream",
                        "Local observation queue exceeded its bound",
                        8,
                    ));
                }
                *previous = Some(sequence);
                self.event_bytes += bytes;
                self.events.push_back((response, bytes));
                continue;
            }
            let id = response["requestId"].as_str().ok_or_else(invalid)?;
            let item = self.pending.get_mut(id).ok_or_else(invalid)?;
            if item.result.is_some()
                || response.get("result").is_some() == response.get("error").is_some()
            {
                return Err(invalid());
            }
            if item.method == "stream.subscribe" && response.get("error").is_none() {
                let subscription = response["result"]["subscriptionId"]
                    .as_str()
                    .ok_or_else(invalid)?
                    .to_owned();
                Identifier::try_from(subscription.clone()).map_err(|_| invalid())?;
                if self.subscriptions.len() >= 8
                    || self.subscriptions.insert(subscription, None).is_some()
                {
                    return Err(invalid());
                }
            }
            let result = if let Some(error) = response.get("error") {
                let error: ProtocolError =
                    serde_json::from_value(error.clone()).map_err(|_| invalid())?;
                let exit = error.exit_category();
                if Identifier::try_from(error.code.clone()).is_err()
                    || Identifier::try_from(error.stage.clone()).is_err()
                {
                    return Err(invalid());
                }
                Err(Box::new(ClientError {
                    code: error.code,
                    stage: error.stage,
                    message: error.message,
                    exit,
                    retryable: error.retryable,
                    operation_id: error.operation_id.map(|i| i.0),
                    deployment_id: error.deployment_id.map(|i| i.0),
                    idempotency_key: item.key.clone(),
                }))
            } else {
                Ok(response["result"].clone())
            };
            item.result = Some(result);
        }
    }
    pub fn next_event(&mut self) -> Option<Value> {
        let (event, bytes) = self.events.pop_front()?;
        self.event_bytes -= bytes;
        Some(event)
    }
    pub fn call(
        &mut self,
        method: &str,
        params: Value,
        key: Option<&str>,
        timeout: Duration,
    ) -> Result<Value, Box<ClientError>> {
        let id = self.submit(method, params, key, timeout)?;
        self.receive(&id)
    }
    /// Retry only a caller-selected operation with its original key on a fresh
    /// authenticated connection. No fresh mutation identity is generated here.
    pub fn replay(
        path: &Path,
        required: &[&str],
        method: &str,
        params: Value,
        key: &str,
        timeout: Duration,
    ) -> Result<Value, Box<ClientError>> {
        let result = Self::connect(path, required, timeout)
            .and_then(|mut client| client.call(method, params, Some(key), timeout));
        result.map_err(|mut error| {
            error.idempotency_key = Some(key.to_owned());
            error
        })
    }
}
fn connect_bounded(path: &Path, timeout: Duration) -> io::Result<UnixStream> {
    use std::os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::ffi::OsStrExt,
    };
    let bytes = path.as_os_str().as_bytes();
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    if bytes.len() >= address.sun_path.len() {
        return Err(io::Error::from(io::ErrorKind::InvalidInput));
    }
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (to, from) in address.sun_path.iter_mut().zip(bytes) {
        *to = *from as libc::c_char;
    }
    #[cfg(target_os = "macos")]
    {
        address.sun_len = std::mem::size_of::<libc::sockaddr_un>() as u8;
    }
    #[cfg(target_os = "linux")]
    let kind = libc::SOCK_STREAM | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK;
    #[cfg(not(target_os = "linux"))]
    let kind = libc::SOCK_STREAM;
    let raw = unsafe { libc::socket(libc::AF_UNIX, kind, 0) };
    if raw < 0 {
        return Err(io::Error::last_os_error());
    }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    #[cfg(target_os = "macos")]
    {
        let enabled: libc::c_int = 1;
        if unsafe {
            libc::setsockopt(
                fd.as_raw_fd(),
                libc::SOL_SOCKET,
                libc::SO_NOSIGPIPE,
                (&enabled as *const libc::c_int).cast(),
                std::mem::size_of_val(&enabled) as libc::socklen_t,
            )
        } < 0
        {
            return Err(io::Error::last_os_error());
        }
    }
    if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) } < 0
        || unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFL, libc::O_NONBLOCK) } < 0
    {
        return Err(io::Error::last_os_error());
    }
    let result = unsafe {
        libc::connect(
            fd.as_raw_fd(),
            (&address as *const libc::sockaddr_un).cast(),
            std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
        )
    };
    if result != 0 {
        let error = io::Error::last_os_error();
        if !matches!(error.raw_os_error(), Some(libc::EINPROGRESS | libc::EAGAIN)) {
            return Err(error);
        }
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or_else(|| io::Error::from(io::ErrorKind::TimedOut))?;
            let mut poll = libc::pollfd {
                fd: fd.as_raw_fd(),
                events: libc::POLLOUT,
                revents: 0,
            };
            let ready =
                unsafe { libc::poll(&mut poll, 1, (remaining.as_millis().max(1)) as libc::c_int) };
            if ready < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(error);
            }
            if ready == 0 {
                return Err(io::Error::from(io::ErrorKind::TimedOut));
            }
            let mut error: libc::c_int = 0;
            let mut length = std::mem::size_of_val(&error) as libc::socklen_t;
            if unsafe {
                libc::getsockopt(
                    fd.as_raw_fd(),
                    libc::SOL_SOCKET,
                    libc::SO_ERROR,
                    (&mut error as *mut libc::c_int).cast(),
                    &mut length,
                )
            } < 0
            {
                return Err(io::Error::last_os_error());
            }
            if error != 0 {
                return Err(io::Error::from_raw_os_error(error));
            }
            break;
        }
    }
    let stream = UnixStream::from(fd);
    // Keep nonblocking: readiness polling applies one absolute deadline across
    // every fragment and permits draining buffered data after peer shutdown.
    stream.set_nonblocking(true)?;
    Ok(stream)
}
