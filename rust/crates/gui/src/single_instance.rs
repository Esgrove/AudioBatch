//! Keeps one app running: a second launch hands its paths to the running app and quits.
//!
//! The running app listens on a loopback TCP port.
//! It writes the port and a random token to a file in the per-user data directory that only the user can read.
//! A launch connects to that port, checks that the app answers with its greeting,
//! and sends the token and then its paths, one per line.
//! The running app drops a connection that does not start with the token,
//! so other users' processes cannot hand it paths, and it limits how much and how long one connection sends.
//! When nothing answers with the greeting, such as after a crash left the port file behind,
//! the launch becomes the running app.
//! A lock file decides between launches that start at the same time, so only one of them becomes the running app.
//! The running app opens the paths it receives as if they were dropped onto its window, and comes to the front.

use std::collections::hash_map::RandomState;
use std::fs::{File, OpenOptions};
use std::hash::{BuildHasher as _, Hasher as _};
use std::io::{BufRead as _, BufReader, Read as _, Write as _};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};

use audiobatch_core::atomic_write::write_atomically;
use audiobatch_core::cache::data_directory;
use crossbeam_channel::{Receiver, unbounded};

/// File in the data directory that holds the port of the running app and its token.
const PORT_FILE_NAME: &str = "gui-instance.port";

/// File in the data directory that launches lock while they decide which of them is the running app.
const LOCK_FILE_NAME: &str = "gui-instance.lock";

/// First line the running app sends, so a launch does not hand its paths to another program on a reused port.
const GREETING: &str = "AudioBatchRs instance";

/// Longest wait for the running app to accept the connection and to answer, and for each line of a launch.
const CONNECT_TIMEOUT: Duration = Duration::from_millis(500);

/// Longest time the running app spends reading one launch, so a slow client cannot block the next one.
const RECEIVE_DEADLINE: Duration = Duration::from_secs(2);

/// Most bytes the running app reads from one launch.
const MAXIMUM_RECEIVED_BYTES: u64 = 1024 * 1024;

/// What the launch does after looking for a running app.
pub enum Claim {
    /// No app was running, so this launch is the app, and receives the paths of later launches here.
    Primary(Receiver<Vec<PathBuf>>),
    /// The running app took the paths, so this launch quits.
    Forwarded,
}

/// Hands the paths to a running app, or makes this launch the running app.
/// Relative paths are resolved against this launch's working directory first.
/// When listening fails, the launch still runs, only without receiving later launches.
pub fn claim(paths: &[PathBuf]) -> Claim {
    let directory = data_directory();
    let port_file = directory.join(PORT_FILE_NAME);
    let absolute: Vec<PathBuf> = paths
        .iter()
        .map(|path| std::path::absolute(path).unwrap_or_else(|_| path.clone()))
        .collect();
    // Held until this launch has either handed over its paths or published its own port.
    let _lock = lock_launches(&directory.join(LOCK_FILE_NAME));
    if forward(&port_file, &absolute) {
        tracing::info!("Handed {} paths to the running app", absolute.len());
        return Claim::Forwarded;
    }
    let (sender, receiver) = unbounded();
    let token = new_token();
    match listen(&port_file, &token) {
        Ok(listener) => {
            let spawned = std::thread::Builder::new()
                .name(String::from("single-instance"))
                .spawn(move || {
                    for stream in listener.incoming().flatten() {
                        if let Some(paths) = receive(stream, &token)
                            && sender.send(paths).is_err()
                        {
                            break;
                        }
                    }
                });
            if let Err(error) = spawned {
                tracing::warn!("Could not start listening for later launches: {error}");
            }
        }
        Err(error) => tracing::warn!("Could not listen for later launches: {error:#}"),
    }
    Claim::Primary(receiver)
}

/// Takes the exclusive lock that launches hold while they decide which of them is the running app.
/// Returns the locked file, which unlocks when dropped, or none when the lock cannot be taken,
/// in which case the launch decides without it.
fn lock_launches(lock_file: &Path) -> Option<File> {
    if let Some(directory) = lock_file.parent() {
        std::fs::create_dir_all(directory).ok()?;
    }
    let file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(lock_file)
        .ok()?;
    file.lock().ok()?;
    Some(file)
}

/// Returns a random token, so only a launch that can read the port file can hand over paths.
fn new_token() -> String {
    let random = RandomState::new();
    let seed = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_nanos());
    let mut parts = [0_u64; 2];
    for (index, part) in parts.iter_mut().enumerate() {
        let mut hasher = random.build_hasher();
        hasher.write_u128(seed);
        hasher.write_u32(std::process::id());
        hasher.write_usize(index);
        *part = hasher.finish();
    }
    format!("{:016x}{:016x}", parts[0], parts[1])
}

/// Sends the token and the paths to the app whose port the file names, and returns true when it took them.
fn forward(port_file: &Path, paths: &[PathBuf]) -> bool {
    let Some((port, token)) = std::fs::read_to_string(port_file)
        .ok()
        .and_then(|text| parse_port_file(&text))
    else {
        return false;
    };
    let address = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    let Ok(mut stream) = TcpStream::connect_timeout(&address, CONNECT_TIMEOUT) else {
        return false;
    };
    if stream.set_read_timeout(Some(CONNECT_TIMEOUT)).is_err() {
        return false;
    }
    let mut greeting = String::new();
    let Ok(reader) = stream.try_clone() else {
        return false;
    };
    if BufReader::new(reader).read_line(&mut greeting).is_err() || greeting.trim_end() != GREETING {
        return false;
    }
    writeln!(stream, "{token}").is_ok() && paths.iter().all(|path| writeln!(stream, "{}", path.display()).is_ok())
}

/// Returns the port and the token of a port file, written as the port and the token on one line.
fn parse_port_file(text: &str) -> Option<(u16, String)> {
    let mut fields = text.split_whitespace();
    let port = fields.next()?.parse().ok()?;
    let token = fields.next()?.to_string();
    Some((port, token))
}

/// Listens on a free loopback port and writes the port and the token to the file,
/// replacing it atomically with a file only the user can read.
fn listen(port_file: &Path, token: &str) -> anyhow::Result<TcpListener> {
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
    let port = listener.local_addr()?.port();
    write_atomically(port_file, format!("{port} {token}").as_bytes())?;
    Ok(listener)
}

/// Greets a launch and reads its paths, one per line, after the token it must send first.
/// Returns none for a connection without the token.
/// Reading stops at the end of the connection, after `MAXIMUM_RECEIVED_BYTES`, or after `RECEIVE_DEADLINE`.
fn receive(mut stream: TcpStream, token: &str) -> Option<Vec<PathBuf>> {
    stream.set_read_timeout(Some(CONNECT_TIMEOUT)).ok()?;
    writeln!(stream, "{GREETING}").ok()?;
    let started = Instant::now();
    let mut lines = BufReader::new(stream.take(MAXIMUM_RECEIVED_BYTES)).lines();
    if lines.next()?.ok()? != token {
        return None;
    }
    let mut paths = Vec::new();
    for line in lines {
        let Ok(line) = line else {
            break;
        };
        if !line.is_empty() {
            paths.push(PathBuf::from(line));
        }
        if started.elapsed() > RECEIVE_DEADLINE {
            break;
        }
    }
    Some(paths)
}

#[cfg(test)]
mod test_single_instance {
    use super::*;

    /// Listens like the running app with a fixed token, and returns the server thread that receives one launch.
    fn serve_once(port_file: &Path) -> std::thread::JoinHandle<Option<Vec<PathBuf>>> {
        let listener = listen(port_file, "secret").expect("listens");
        std::thread::spawn(move || {
            let (stream, _) = listener.accept().expect("accepts");
            receive(stream, "secret")
        })
    }

    #[test]
    fn a_second_launch_hands_its_paths_to_the_first() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let port_file = directory.path().join(PORT_FILE_NAME);
        let server = serve_once(&port_file);
        let paths = [PathBuf::from("/music/a.wav"), PathBuf::from("/music/folder")];
        assert!(forward(&port_file, &paths));
        assert_eq!(server.join().expect("the server thread ends"), Some(paths.to_vec()));
    }

    #[test]
    fn a_connection_without_the_token_is_dropped() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let port_file = directory.path().join(PORT_FILE_NAME);
        let server = serve_once(&port_file);
        let (port, _) = parse_port_file(&std::fs::read_to_string(&port_file).expect("read")).expect("parses");
        let mut stream = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).expect("connects");
        writeln!(stream, "wrong token\n/music/a.wav").expect("writes");
        drop(stream);
        assert_eq!(server.join().expect("the server thread ends"), None);
    }

    #[test]
    fn a_stale_or_foreign_port_does_not_take_the_paths() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let port_file = directory.path().join(PORT_FILE_NAME);
        assert!(!forward(&port_file, &[]));

        let foreign = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).expect("binds");
        let port = foreign.local_addr().expect("has an address").port();
        std::fs::write(&port_file, format!("{port} token")).expect("written");
        let server = std::thread::spawn(move || {
            let (mut stream, _) = foreign.accept().expect("accepts");
            writeln!(stream, "Some other program").expect("writes");
        });
        assert!(!forward(&port_file, &[PathBuf::from("/music/a.wav")]));
        server.join().expect("the server thread ends");
    }

    #[test]
    fn tokens_differ_between_calls() {
        assert_ne!(new_token(), new_token());
        assert_eq!(new_token().len(), 32);
    }

    #[test]
    fn the_launch_lock_serializes_launches() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let lock_file = directory.path().join(LOCK_FILE_NAME);
        let first = lock_launches(&lock_file).expect("locks");
        let second = File::open(&lock_file).expect("opens");
        assert!(second.try_lock().is_err());
        drop(first);
        assert!(second.try_lock().is_ok());
    }
}
