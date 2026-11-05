//! Native worker code library. No executable, ambient auth, cache activation or DB access.
#[cfg(target_os = "linux")]
mod acquire;
#[cfg(target_os = "linux")]
mod offline;
mod plan;
#[cfg(target_os = "linux")]
mod resume;
#[cfg(any(target_os = "linux", test))]
mod transport;
#[cfg(any(target_os = "linux", test))]
mod unpack;
use oci_client::secrets::RegistryAuth;
use plan::Plan;
#[cfg(target_os = "linux")]
use std::fs::File;

#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Ok = 0,
    Invalid = 1,
    Unavailable = 2,
    Transfer = 3,
    Integrity = 4,
    Quota = 5,
    Unpack = 6,
    Panic = 7,
}
type Result<T> = std::result::Result<T, Status>;
#[repr(C)]
pub struct Auth {
    kind: u32,
    username: *const u8,
    username_length: usize,
    secret: *const u8,
    secret_length: usize,
}
#[repr(C)]
#[derive(Default)]
pub struct Outcome {
    downloaded_bytes: u64,
    merged_bytes: u64,
}
unsafe fn borrowed<'a>(pointer: *const u8, length: usize, maximum: usize) -> Result<&'a [u8]> {
    if length > maximum || (length != 0 && pointer.is_null()) {
        return Err(Status::Invalid);
    }
    if length == 0 {
        return Ok(&[]);
    }
    Ok(unsafe { std::slice::from_raw_parts(pointer, length) })
}
unsafe fn auth(value: *const Auth) -> Result<RegistryAuth> {
    if value.is_null() {
        return Err(Status::Invalid);
    }
    let a = unsafe { &*value };
    match a.kind {
        0 if a.username_length == 0 && a.secret_length == 0 => Ok(RegistryAuth::Anonymous),
        1 if a.username_length > 0 && a.secret_length > 0 => {
            let user =
                std::str::from_utf8(unsafe { borrowed(a.username, a.username_length, 4096) }?)
                    .map_err(|_| Status::Invalid)?;
            let secret =
                std::str::from_utf8(unsafe { borrowed(a.secret, a.secret_length, 65536) }?)
                    .map_err(|_| Status::Invalid)?;
            if user.contains(':') || user.contains(['\r', '\n', '\0']) {
                return Err(Status::Invalid);
            }
            Ok(RegistryAuth::Basic(user.to_owned(), secret.to_owned()))
        }
        _ => Err(Status::Invalid),
    }
}
#[cfg(target_os = "linux")]
fn root(fd: i32) -> Result<(File, std::path::PathBuf)> {
    use std::os::{
        fd::{AsRawFd, FromRawFd},
        unix::fs::MetadataExt,
    };
    // No parent path lookup: duplicate the owner's already-open directory.
    let duplicate = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 3) };
    if duplicate < 0 {
        return Err(Status::Invalid);
    }
    let file = unsafe { File::from_raw_fd(duplicate) };
    let metadata = file.metadata().map_err(|_| Status::Invalid)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o7777 != 0o700
    {
        return Err(Status::Invalid);
    }
    let path = std::path::PathBuf::from(format!("/proc/self/fd/{}", file.as_raw_fd()));
    if std::fs::read_dir(&path)
        .map_err(|_| Status::Invalid)?
        .next()
        .is_some()
    {
        return Err(Status::Invalid);
    }
    Ok((file, path))
}
#[cfg(target_os = "linux")]
fn worker_limits() -> Result<()> {
    let mut core = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    if unsafe { libc::getrlimit(libc::RLIMIT_CORE, &mut core) } != 0 || core.rlim_cur != 0 {
        return Err(Status::Unavailable);
    }
    #[cfg(feature = "instrumented-host")]
    const MAXIMUM_ADDRESS_SPACE: u64 = 64u64 * 1024 * 1024 * 1024 * 1024;
    #[cfg(not(feature = "instrumented-host"))]
    const MAXIMUM_ADDRESS_SPACE: u64 = 1024u64 * 1024 * 1024;
    for (kind, maximum) in [
        (libc::RLIMIT_AS, MAXIMUM_ADDRESS_SPACE),
        (libc::RLIMIT_FSIZE, 8u64 * 1024 * 1024 * 1024),
    ] {
        let mut limit = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if unsafe { libc::getrlimit(kind, &mut limit) } != 0
            || limit.rlim_cur == libc::RLIM_INFINITY
            || limit.rlim_cur > maximum
        {
            return Err(Status::Unavailable);
        }
    }
    if unsafe { libc::geteuid() } == 0 {
        return Err(Status::Unavailable);
    }
    Ok(())
}
fn quiet_panics() {
    static HOOK: std::sync::Once = std::sync::Once::new();
    HOOK.call_once(|| std::panic::set_hook(Box::new(|_| {})));
}
/// Stage a trusted registered image in an isolated, resource-limited native worker.
/// # Safety
/// Buffers/structs must be valid and remain borrowed for this entire call, result
/// must be writable and disjoint, and staging_fd must be an owned directory.
/// Call only in the worker, with parent-enforced deadline and disk reservation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gitstored_image_stage_v1(
    plan_bytes: *const u8,
    plan_length: usize,
    staging_fd: i32,
    credentials: *const Auth,
    result: *mut Outcome,
) -> Status {
    quiet_panics();
    if result.is_null() {
        return Status::Invalid;
    }
    unsafe { result.write(Outcome::default()) };
    let outcome = std::panic::catch_unwind(|| -> Result<Outcome> {
        let plan = Plan::parse(unsafe { borrowed(plan_bytes, plan_length, 65536) }?)?;
        let credentials = unsafe { auth(credentials) }?;
        #[cfg(target_os = "linux")]
        {
            worker_limits()?;
            let (_directory, path) = root(staging_fd)?;
            acquire::stage(&plan, &path, &credentials)
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = (plan, credentials, staging_fd);
            Err(Status::Unavailable)
        }
    });
    match outcome {
        Ok(Ok(value)) => {
            unsafe { result.write(value) };
            Status::Ok
        }
        Ok(Err(status)) => status,
        Err(_) => Status::Panic,
    }
}

/// Import a pinned OCI layout without creating a network client.
/// # Safety
/// Buffers must remain valid for this call, result must be writable and disjoint,
/// and source_fd/staging_fd must remain open. Requires the same isolated worker
/// limits and parent-enforced deadline as gitstored_image_stage_v1.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gitstored_image_import_v1(
    plan_bytes: *const u8,
    plan_length: usize,
    source_fd: i32,
    staging_fd: i32,
    result: *mut Outcome,
) -> Status {
    unsafe {
        import_source(
            plan_bytes,
            plan_length,
            source_fd,
            staging_fd,
            result,
            false,
        )
    }
}
/// Reconstruct a private workload root from verified cache blobs without HTTP.
/// # Safety
/// Same borrowing and isolated-worker requirements as image_import_v1; source
/// must be the parent's pinned managed cache and the destination independently owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gitstored_image_materialize_v1(
    plan_bytes: *const u8,
    plan_length: usize,
    source_fd: i32,
    staging_fd: i32,
    result: *mut Outcome,
) -> Status {
    unsafe { import_source(plan_bytes, plan_length, source_fd, staging_fd, result, true) }
}
unsafe fn import_source(
    plan_bytes: *const u8,
    plan_length: usize,
    source_fd: i32,
    staging_fd: i32,
    result: *mut Outcome,
    cached: bool,
) -> Status {
    quiet_panics();
    if result.is_null() {
        return Status::Invalid;
    }
    unsafe { result.write(Outcome::default()) };
    let outcome = std::panic::catch_unwind(|| -> Result<Outcome> {
        let plan = Plan::parse(unsafe { borrowed(plan_bytes, plan_length, 65536) }?)?;
        #[cfg(target_os = "linux")]
        {
            worker_limits()?;
            let (_directory, path) = root(staging_fd)?;
            if cached {
                offline::stage_cache(&plan, source_fd, &path)
            } else {
                offline::stage(&plan, source_fd, &path)
            }
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = (plan, source_fd, staging_fd, cached);
            Err(Status::Unavailable)
        }
    });
    match outcome {
        Ok(Ok(value)) => {
            unsafe { result.write(value) };
            Status::Ok
        }
        Ok(Err(status)) => status,
        Err(_) => Status::Panic,
    }
}

/// Resume only complete digest-verified blobs into fresh owned staging.
/// # Safety
/// Same buffer, descriptor, worker-limit and deadline requirements as stage_v1.
/// cached_fd must be a pinned owned prior staging root whose producer stopped;
/// the parent must separately authorize the immutable registry plan and source.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn gitstored_image_resume_v1(
    plan_bytes: *const u8,
    plan_length: usize,
    cached_fd: i32,
    staging_fd: i32,
    credentials: *const Auth,
    result: *mut Outcome,
) -> Status {
    quiet_panics();
    if result.is_null() {
        return Status::Invalid;
    }
    unsafe { result.write(Outcome::default()) };
    let outcome = std::panic::catch_unwind(|| -> Result<Outcome> {
        let plan = Plan::parse(unsafe { borrowed(plan_bytes, plan_length, 65536) }?)?;
        let credentials = unsafe { auth(credentials) }?;
        #[cfg(target_os = "linux")]
        {
            worker_limits()?;
            let (_directory, path) = root(staging_fd)?;
            let cached = resume::CachedBlobs::new(&plan, cached_fd, &path)?;
            acquire::stage_cached(&plan, &path, &credentials, Some(&cached))
        }
        #[cfg(not(target_os = "linux"))]
        {
            let _ = (plan, credentials, cached_fd, staging_fd);
            Err(Status::Unavailable)
        }
    });
    match outcome {
        Ok(Ok(value)) => {
            unsafe { result.write(value) };
            Status::Ok
        }
        Ok(Err(status)) => status,
        Err(_) => Status::Panic,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(target_os = "linux")]
    #[test]
    fn worker_address_bounds_are_finite_and_build_specific() {
        if unsafe { libc::geteuid() } == 0 {
            assert!(matches!(worker_limits(), Err(Status::Unavailable)));
            return;
        }
        const GIB: u64 = 1024 * 1024 * 1024;
        for (address_space, accepted) in [
            (libc::RLIM_INFINITY, false),
            (GIB, true),
            (2 * GIB, cfg!(feature = "instrumented-host")),
            (64 * 1024 * GIB, cfg!(feature = "instrumented-host")),
            (64 * 1024 * GIB + 1, false),
        ] {
            // The child performs only limit syscalls and the allocation-free
            // guard, preserving all limits of the parallel test runner.
            let child = unsafe { libc::fork() };
            assert!(child >= 0);
            if child == 0 {
                let mut valid = true;
                for (kind, value) in [
                    (libc::RLIMIT_CORE, 0),
                    (libc::RLIMIT_FSIZE, 8 * GIB),
                    (libc::RLIMIT_AS, address_space),
                ] {
                    let limit = libc::rlimit {
                        rlim_cur: value,
                        rlim_max: value,
                    };
                    valid &= unsafe { libc::setrlimit(kind, &limit) } == 0;
                }
                unsafe { libc::_exit(i32::from(!valid || worker_limits().is_ok() != accepted)) };
            }
            let mut status = 0;
            assert_eq!(unsafe { libc::waitpid(child, &mut status, 0) }, child);
            assert!(libc::WIFEXITED(status));
            assert_eq!(libc::WEXITSTATUS(status), 0);
        }
    }
    #[test]
    fn ffi_rejects_invalid_before_files_or_network() {
        let mut result = Outcome {
            downloaded_bytes: 999,
            merged_bytes: 999,
        };
        let status = unsafe {
            gitstored_image_stage_v1(std::ptr::null(), 2, -1, std::ptr::null(), &mut result)
        };
        assert_eq!(status, Status::Invalid);
        assert_eq!(result.downloaded_bytes, 0);
        assert_eq!(result.merged_bytes, 0);
    }
    #[cfg(target_os = "linux")]
    #[test]
    fn directory_descriptor_pins_owned_staging_and_remains_borrowed() {
        use std::os::{fd::AsRawFd, unix::fs::PermissionsExt};
        let fixture = tempfile::tempdir().unwrap();
        let original = fixture.path().join("staging");
        let renamed = fixture.path().join("renamed");
        std::fs::create_dir(&original).unwrap();
        std::fs::set_permissions(&original, std::fs::Permissions::from_mode(0o700)).unwrap();
        let borrowed = File::open(&original).unwrap();
        let (duplicate, pinned) = root(borrowed.as_raw_fd()).unwrap();
        assert_ne!(
            unsafe { libc::fcntl(duplicate.as_raw_fd(), libc::F_GETFD) } & libc::FD_CLOEXEC,
            0
        );
        std::fs::rename(&original, &renamed).unwrap();
        std::fs::write(pinned.join("owned"), b"synthetic").unwrap();
        assert_eq!(std::fs::read(renamed.join("owned")).unwrap(), b"synthetic");
        drop(duplicate);
        assert!(borrowed.metadata().unwrap().is_dir());
        assert!(
            matches!(root(borrowed.as_raw_fd()), Err(Status::Invalid)),
            "non-empty staging must not be reused"
        );
        std::fs::remove_file(renamed.join("owned")).unwrap();
        std::fs::set_permissions(&renamed, std::fs::Permissions::from_mode(0o755)).unwrap();
        assert!(
            matches!(root(borrowed.as_raw_fd()), Err(Status::Invalid)),
            "shared directory must be refused"
        );
    }
}
