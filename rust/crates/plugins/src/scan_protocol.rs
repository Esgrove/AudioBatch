//! The messages between a scan and its worker process.
//!
//! Each request is one JSON line on the worker's stdin.
//! Each reply is one line on its stdout that contains a marker followed by JSON.
//! Plugins can print to stdout too, even in the middle of a line,
//! so the parent looks for the marker anywhere in a line and ignores lines without it.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::library::PluginDescription;

/// Command line flag that starts an executable as a scan worker.
pub const WORKER_FLAG: &str = "--audiobatch-plugin-scan-worker";

/// Text that starts a reply on the worker's stdout.
const REPLY_MARKER: &str = "AUDIOBATCH-SCAN-REPLY ";

/// A request to scan one bundle.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScanRequest {
    /// Path of the bundle.
    pub bundle: PathBuf,
}

/// The result of scanning one bundle.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScanReply {
    /// The audio effect classes in the bundle.
    #[serde(default)]
    pub plugins: Vec<PluginDescription>,
    /// Why the bundle could not be loaded.
    #[serde(default)]
    pub error: Option<String>,
}

impl ScanReply {
    /// Returns the reply line, with the marker and without a line break.
    pub fn to_line(&self) -> String {
        let json = serde_json::to_string(self).unwrap_or_else(|_| "{}".to_owned());
        format!("{REPLY_MARKER}{json}")
    }

    /// Returns the reply in a line of worker output, or `None` when the line holds no reply.
    pub fn from_line(line: &str) -> Option<Self> {
        let start = line.find(REPLY_MARKER)? + REPLY_MARKER.len();
        serde_json::from_str(line.get(start..)?).ok()
    }
}

#[cfg(test)]
mod test_scan_protocol {
    use super::*;

    #[test]
    fn reply_round_trips_through_a_line() {
        let reply = ScanReply {
            plugins: Vec::new(),
            error: Some("Could not load".to_owned()),
        };
        assert_eq!(ScanReply::from_line(&reply.to_line()), Some(reply));
    }

    #[test]
    fn plugin_output_before_the_marker_is_ignored() {
        let reply = ScanReply::default();
        let line = format!("plugin says hello{}", reply.to_line());
        assert_eq!(ScanReply::from_line(&line), Some(reply));
    }

    #[test]
    fn lines_without_a_valid_reply_are_ignored() {
        for line in [
            "",
            "plugin output",
            "AUDIOBATCH-SCAN-REPLY",
            "AUDIOBATCH-SCAN-REPLY ",
            "AUDIOBATCH-SCAN-REPLY {broken",
            "AUDIOBATCH-SCAN-REPLY {\"plugins\": [",
            "AUDIOBATCH-SCAN-REPLY {\"plugins\": 5}",
            "AUDIOBATCH-SCAN-REPLY \"text\"",
            "AUDIOBATCH-SCAN-REPLY {} trailing plugin output",
            "{\"plugins\": [], \"error\": null}",
        ] {
            assert_eq!(ScanReply::from_line(line), None, "{line:?}");
        }
    }

    #[test]
    fn missing_fields_default_and_a_carriage_return_is_ignored() {
        assert_eq!(
            ScanReply::from_line("AUDIOBATCH-SCAN-REPLY {}\r"),
            Some(ScanReply::default())
        );
        assert_eq!(
            ScanReply::from_line("AUDIOBATCH-SCAN-REPLY {\"error\": \"Failed\", \"unknown\": 1}"),
            Some(ScanReply {
                plugins: Vec::new(),
                error: Some("Failed".to_owned()),
            })
        );
    }
}
