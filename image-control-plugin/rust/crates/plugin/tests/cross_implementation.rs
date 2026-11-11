//! Invokes the independent JUCE host against both explicit build-tree VST3 implementations.
//! The host owns deterministic inputs, analytical expectations, and full-scale residual acceptance limits.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

#[path = "../examples/support/artifacts.rs"]
mod artifacts;

/// Bounds the complete native host matrix, including plugin load, offline rendering, and teardown.
const PARITY_TIMEOUT: Duration = Duration::from_secs(300);
/// Every selected fixture is required together so a typo never silently disables the comparison.
const VARIABLES: [&str; 3] = [
    "IMAGE_CONTROL_PARITY_EXECUTABLE",
    "IMAGE_CONTROL_CPP_BUNDLE",
    "IMAGE_CONTROL_OWN_BUNDLE",
];

/// Validates an explicit build artifact without falling back to installed plugin search paths.
fn artifact(value: std::ffi::OsString, name: &str) -> PathBuf {
    let path = PathBuf::from(value);
    assert!(path.is_absolute(), "{name} must be absolute");
    artifacts::checked_artifact(&path).expect("selected parity artifact must be inside an authorized build root")
}

/// Runs actual packaged processing when the paired fixtures are selected and makes absent local fixtures explicit.
#[test]
fn packaged_plugins_match_independent_signal_contracts() {
    let values = VARIABLES.map(std::env::var_os);
    // OWN_BUNDLE alone is also used by the ordinary single-plugin packaged contracts.
    // A parity executable or C++ counterpart selects this stricter three-artifact integration contract.
    let selected = values[0].is_some() || values[1].is_some();
    if !selected {
        assert!(
            std::env::var_os("IMAGE_CONTROL_PARITY_REQUIRED").is_none(),
            "required parity needs IMAGE_CONTROL_PARITY_EXECUTABLE, IMAGE_CONTROL_CPP_BUNDLE, and IMAGE_CONTROL_OWN_BUNDLE"
        );
        eprintln!(
            "SKIP cross-implementation parity: set IMAGE_CONTROL_PARITY_EXECUTABLE, IMAGE_CONTROL_CPP_BUNDLE, \
             and IMAGE_CONTROL_OWN_BUNDLE to explicit build-tree artifacts"
        );
        return;
    }
    assert!(
        values.iter().all(Option::is_some),
        "partial parity configuration: all three artifacts are required"
    );
    let repository = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../..")
        .canonicalize()
        .expect("repository");
    let paths: Vec<_> = values
        .into_iter()
        .zip(VARIABLES)
        .map(|(value, name)| artifact(value.expect("checked configuration"), name))
        .collect();
    assert!(paths[0].is_file(), "parity executable must be a file");
    for path in &paths[1..] {
        assert!(
            path.is_dir() && path.extension().is_some_and(|extension| extension == "vst3"),
            "counterparts must be explicit packaged VST3 directories"
        );
    }
    let target = std::env::var_os("CARGO_TARGET_DIR")
        .map_or_else(|| repository.join("rust/target"), |path| repository.join(path));
    let directory = target.join("image-control-parity");
    std::fs::create_dir_all(&directory).expect("parity artifact directory");
    let report = directory.join(format!("plugin-parity-{}.json", std::process::id()));
    // Inherited streams cannot fill a pipe while the parent is polling the bounded child process.
    let mut child = Command::new(&paths[0])
        .arg("--cpp")
        .arg(&paths[1])
        .arg("--rust")
        .arg(&paths[2])
        .arg("--report")
        .arg(&report)
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .expect("start independent parity executable");
    let deadline = Instant::now() + PARITY_TIMEOUT;
    let status = loop {
        if let Some(status) = child.try_wait().expect("poll parity executable") {
            break status;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            assert!(
                Instant::now() < deadline,
                "cross-implementation parity exceeded its 300-second deadline"
            );
        }
        thread::sleep(Duration::from_millis(20));
    };
    let text = std::fs::read_to_string(&report).expect("parity host must emit its per-case report");
    let report: serde_json::Value = serde_json::from_str(&text).expect("valid parity report JSON");
    assert_eq!(report["schema_version"], 1);
    assert_eq!(report["recover_common_max_percent"], 100.0);
    assert_eq!(report["capability_differences"][0]["cpp_max_percent"], 200.0);
    assert_eq!(report["capability_differences"][0]["rust_max_percent"], 100.0);
    assert_eq!(report["quick"], false, "integration must execute the full matrix");
    assert!(report["cases"].as_array().is_some_and(|cases| !cases.is_empty()));
    let failures: Vec<_> = report["cases"]
        .as_array()
        .expect("case list")
        .iter()
        .filter(|case| case["passed"] != true)
        .map(|case| {
            serde_json::json!({
                "identity": case["identity"],
                "error": case["error"],
                "residual": case["residual"],
                "cpp_errors": case["cpp"]["errors"],
                "rust_errors": case["rust"]["errors"],
            })
        })
        .collect();
    assert!(status.success(), "parity host failed: {status}\n{failures:#?}");
    assert_eq!(
        report["failed"], 0,
        "every numerical/domain failure must fail integration"
    );
}
