//! Keeps one app running: a second launch hands its paths to the running app and quits.
//!
//! The running app listens on a loopback TCP port and writes the port to a file in the data directory.
//! A launch connects to that port, checks that the app answers with its greeting, and sends its paths, one per line.
//! When nothing answers with the greeting, such as after a crash left the port file behind,
//! the launch becomes the running app.
//! The running app opens the paths it receives as if they were dropped onto its window, and comes to the front.

use std::io::{BufRead as _, BufReader, Write as _};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

use audiobatch_core::cache::data_directory;
use crossbeam_channel::{Receiver, unbounded};

/// File in the data directory that holds the port of the running app.
const PORT_FILE_NAME: &str = "gui-instance.port";

/// First line the running app sends, so a launch does not hand its paths to another program on a reused port.
const GREETING: &str = "AudioBatchRs instance";

/// Longest wait for the running app to accept the connection and to answer.
const CONNECT_TIMEOUT: Duration = Duration::from_millis(500);

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
    let port_file = data_directory().join(PORT_FILE_NAME);
    let absolute: Vec<PathBuf> = paths
        .iter()
        .map(|path| std::path::absolute(path).unwrap_or_else(|_| path.clone()))
        .collect();
    if forward(&port_file, &absolute) {
        tracing::info!("Handed {} paths to the running app", absolute.len());
        return Claim::Forwarded;
    }
    let (sender, receiver) = unbounded();
    match listen(&port_file) {
        Ok(listener) => {
            let spawned = std::thread::Builder::new()
                .name(String::from("single-instance"))
                .spawn(move || {
                    for stream in listener.incoming().flatten() {
                        if let Some(paths) = receive(stream)
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

/// Sends the paths to the app whose port the file names, and returns true when it took them.
fn forward(port_file: &Path, paths: &[PathBuf]) -> bool {
    let Some(port) = std::fs::read_to_string(port_file)
        .ok()
        .and_then(|text| text.trim().parse::<u16>().ok())
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
    paths.iter().all(|path| writeln!(stream, "{}", path.display()).is_ok())
}

/// Listens on a free loopback port and writes the port to the file.
fn listen(port_file: &Path) -> anyhow::Result<TcpListener> {
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))?;
    let port = listener.local_addr()?.port();
    if let Some(directory) = port_file.parent() {
        std::fs::create_dir_all(directory)?;
    }
    std::fs::write(port_file, port.to_string())?;
    Ok(listener)
}

/// Greets a launch and reads its paths, one per line, until it closes the connection.
fn receive(mut stream: TcpStream) -> Option<Vec<PathBuf>> {
    stream.set_read_timeout(Some(CONNECT_TIMEOUT)).ok()?;
    writeln!(stream, "{GREETING}").ok()?;
    let paths = BufReader::new(stream)
        .lines()
        .map_while(Result::ok)
        .filter(|line| !line.is_empty())
        .map(PathBuf::from)
        .collect();
    Some(paths)
}

#[cfg(test)]
mod test_single_instance {
    use super::*;

    #[test]
    fn a_second_launch_hands_its_paths_to_the_first() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let port_file = directory.path().join(PORT_FILE_NAME);
        let listener = listen(&port_file).expect("listens");
        let server = std::thread::spawn(move || {
            let (stream, _) = listener.accept().expect("accepts");
            receive(stream).expect("receives")
        });
        let paths = [PathBuf::from("/music/a.wav"), PathBuf::from("/music/folder")];
        assert!(forward(&port_file, &paths));
        assert_eq!(server.join().expect("the server thread ends"), paths);
    }

    #[test]
    fn a_stale_or_foreign_port_does_not_take_the_paths() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let port_file = directory.path().join(PORT_FILE_NAME);
        assert!(!forward(&port_file, &[]));

        let foreign = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).expect("binds");
        let port = foreign.local_addr().expect("has an address").port();
        std::fs::write(&port_file, port.to_string()).expect("written");
        let server = std::thread::spawn(move || {
            let (mut stream, _) = foreign.accept().expect("accepts");
            writeln!(stream, "Some other program").expect("writes");
        });
        assert!(!forward(&port_file, &[PathBuf::from("/music/a.wav")]));
        server.join().expect("the server thread ends");
    }
}
