//! Bundler argument, native layout, build-planning, and isolated artifact contract tests.

use std::ffi::{OsStr, OsString};
use std::path::Path;
use std::process::Command;
use std::time::{Duration, SystemTime};

use crate::arguments::{FormatSelection, Options};
use crate::{
    Architecture, BuildContext, PLUGIN_NAME, Platform, PluginFormat, Profile, absolute_target_directory,
    build_commands, context_from_metadata, package,
};

/// Creates a representative native context without changing environment variables or production storage.
fn context(directory: &Path, platform: Platform, profile: Profile) -> BuildContext {
    BuildContext {
        workspace: directory.join("workspace with spaces"),
        target: directory.join("artifacts with spaces"),
        version: "0.1.0".into(),
        platform,
        architecture: Architecture::Arm64,
        profile,
    }
}

/// Uses the real parser with its fixed command prefix.
fn parse(arguments: &[&str]) -> Result<Option<Options>, Box<dyn std::error::Error>> {
    let arguments = ["bundle", "image-control-plugin-rs"]
        .into_iter()
        .chain(arguments.iter().copied())
        .map(str::to_owned)
        .collect::<Vec<_>>();
    Options::parse(&arguments)
}

/// Extracts lossless arguments for native path and command-plan assertions.
fn args(command: &Command) -> Vec<OsString> {
    command.get_args().map(OsStr::to_os_string).collect()
}

/// Creates a nonempty fake artifact. Packaging tests copy bytes rather than loading a plugin.
fn write_artifact(path: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(path.parent().expect("artifact parent")).expect("create artifact directory");
    std::fs::write(path, bytes).expect("write artifact");
}

/// Creates exactly the sibling `CMake` output and matching cache expected by the real validator.
fn write_audio_unit(context: &BuildContext) {
    write_artifact(&context.static_library(), b"rust static archive");
    let cache = format!(
        "CMAKE_BUILD_TYPE:STRING={}\nCMAKE_OSX_ARCHITECTURES:STRING={}\n\
         CMAKE_OSX_DEPLOYMENT_TARGET:STRING=15.0\nIMAGE_CONTROL_RUST_LIBRARY:FILEPATH={}\n\
         IMAGE_CONTROL_VERSION:STRING={}\n",
        context.profile.cmake_name(),
        context.architecture.cmake_name(),
        context.static_library().display(),
        context.version
    );
    write_artifact(&context.au_build().join("CMakeCache.txt"), cache.as_bytes());
    let bundle = context.au_bundle();
    write_artifact(&bundle.join("Contents/Info.plist"), b"native AU metadata");
    let executable = context
        .executable(&bundle, PluginFormat::AudioUnit)
        .expect("AU executable");
    write_artifact(&executable, b"native AU executable");
    let older = SystemTime::now()
        .checked_sub(Duration::from_secs(120))
        .expect("older timestamp");
    std::fs::File::options()
        .write(true)
        .open(context.static_library())
        .expect("archive")
        .set_times(std::fs::FileTimes::new().set_modified(older))
        .expect("set archive timestamp");
}

/// A relative override retains the caller's directory instead of the nested Cargo working directory.
#[test]
fn relative_target_override_is_anchored_before_changing_directory() {
    let invocation = std::env::current_dir().expect("invocation directory");
    let target = Path::new("custom artifacts with spaces");
    assert_eq!(absolute_target_directory(target, &invocation), invocation.join(target));
}

/// An absolute custom directory remains independent of the invoking shell's location.
#[test]
fn absolute_target_override_is_not_rebased() {
    let invocation = std::env::current_dir().expect("invocation directory");
    let target = std::env::temp_dir().join("ImageControl external build");
    assert_eq!(absolute_target_directory(&target, &invocation), target);
}

/// Existing unqualified invocations build Debug and select all supported native formats.
#[test]
fn defaults_select_debug_and_all_formats() {
    assert_eq!(
        parse(&[]).expect("default options"),
        Some(Options {
            profile: Profile::Debug,
            no_build: false,
            formats: FormatSelection::All
        })
    );
}

/// Short and long options drive the same parser state rather than help-only aliases.
#[test]
fn short_and_long_options_match() {
    assert_eq!(
        parse(&["-r", "-N", "-f", "clap"]).expect("short options"),
        parse(&["--release", "--no-build", "--format", "clap"]).expect("long options")
    );
    assert_eq!(
        parse(&["-d", "-f", "au"]).expect("short options"),
        parse(&["--debug", "--format=au"]).expect("long options")
    );
}

/// Help exits before validating a command prefix or needing native tools and paths.
#[test]
fn help_is_side_effect_free_even_with_incomplete_arguments() {
    for help in ["-h", "--help"] {
        assert!(Options::parse(&[help.to_owned()]).expect("help").is_none());
        assert!(
            parse(&["--format", help])
                .expect("help after incomplete option")
                .is_none()
        );
    }
}

/// Misspelled, missing, conflicting, and repeated selectors are actionable errors.
#[test]
fn invalid_arguments_are_rejected() {
    for arguments in [
        vec!["--format"],
        vec!["-f", "unknown"],
        vec!["--format="],
        vec!["--unknown"],
        vec!["--release", "--debug"],
        vec!["--format", "clap", "--format", "vst3"],
        vec!["vst3"],
    ] {
        assert!(parse(&arguments).is_err(), "accepted invalid arguments: {arguments:?}");
    }
    assert!(Options::parse(&[]).is_err());
    assert!(Options::parse(&["bundle".into(), "different-package".into()]).is_err());
}

/// All expands to supported formats and an explicit AU is never silently omitted on Windows.
#[test]
fn format_selection_is_platform_specific() {
    assert_eq!(
        FormatSelection::All.select(Platform::MacOs).expect("macOS formats"),
        vec![PluginFormat::Vst3, PluginFormat::Clap, PluginFormat::AudioUnit]
    );
    assert_eq!(
        FormatSelection::All.select(Platform::Windows).expect("Windows formats"),
        vec![PluginFormat::Vst3, PluginFormat::Clap]
    );
    assert!(
        FormatSelection::One(PluginFormat::AudioUnit)
            .select(Platform::Windows)
            .is_err()
    );
    for name in ["vst3", "clap", "au"] {
        let options = parse(&["-f", name]).expect("format argument").expect("options");
        assert_eq!(options.formats.select(Platform::MacOs).expect("one format").len(), 1);
    }
}

/// Cargo metadata is authoritative for the custom artifact root and package version.
#[test]
fn metadata_selects_the_plugin_package_and_absolute_paths() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let metadata = serde_json::json!({
        "workspace_root": directory.path(),
        "target_directory": directory.path().join("custom artifacts"),
        "packages": [
            {"name": "unrelated", "version": "99.0.0"},
            {"name": "image-control-plugin-rs", "version": "0.1.0"}
        ]
    });
    let context = context_from_metadata(&metadata, Platform::MacOs, Architecture::Arm64, Profile::Release)
        .expect("workspace context");
    assert_eq!(context.target, directory.path().join("custom artifacts"));
    assert_eq!(context.version, "0.1.0");
    let mut relative = metadata;
    relative["target_directory"] = "relative artifacts".into();
    assert!(context_from_metadata(&relative, Platform::MacOs, Architecture::Arm64, Profile::Debug).is_err());
}

/// Both macOS dynamic formats retain their conventional bundle layout and separate public paths.
#[test]
fn macos_bundle_paths_are_exact() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Release);
    for format in [PluginFormat::Vst3, PluginFormat::Clap, PluginFormat::AudioUnit] {
        let bundle = context.bundle(format);
        assert_eq!(
            bundle,
            context
                .target
                .join("bundled")
                .join(format!("{PLUGIN_NAME}.{}", format.extension()))
        );
        assert_eq!(
            context.executable(&bundle, format).expect("macOS executable"),
            bundle.join("Contents/MacOS/ImageControl RS")
        );
    }
    assert_eq!(
        context.dynamic_library(),
        context.target.join("release/libimage_control_plugin_rs.dylib")
    );
    assert_eq!(
        context.static_library(),
        context.target.join("release/libimage_control_plugin_rs.a")
    );
}

/// Windows CLAP is a single file while VST3 preserves its architecture-specific executable directory.
#[test]
fn windows_bundle_paths_cover_both_architectures() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let mut context = context(directory.path(), Platform::Windows, Profile::Debug);
    assert_eq!(
        context.dynamic_library(),
        context.target.join("debug/image_control_plugin_rs.dll")
    );
    for (architecture, folder) in [(Architecture::Arm64, "arm64-win"), (Architecture::X86_64, "x86_64-win")] {
        context.architecture = architecture;
        let vst3 = context.bundle(PluginFormat::Vst3);
        assert_eq!(
            context.executable(&vst3, PluginFormat::Vst3).expect("VST3 executable"),
            vst3.join("Contents").join(folder).join("ImageControl RS.vst3")
        );
        let clap = context.bundle(PluginFormat::Clap);
        assert_eq!(
            context.executable(&clap, PluginFormat::Clap).expect("CLAP executable"),
            clap
        );
        assert!(context.executable(&clap, PluginFormat::AudioUnit).is_err());
    }
}

/// No-build has no Cargo compilation, `CMake` configure, `CMake` build, or format-specific exception.
#[test]
fn no_build_never_plans_compilation_commands() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Release);
    for selection in ["all", "vst3", "clap", "au"] {
        let options = parse(&["--release", "-N", "-f", selection])
            .expect("arguments")
            .expect("options");
        let formats = options.formats.select(context.platform).expect("formats");
        assert!(build_commands(&context, options, &formats).is_empty());
    }
}

/// AU configuration receives absolute archive paths, package version, native architecture, and matching profiles.
#[test]
fn au_commands_match_the_rust_build_and_sibling_cmake_contract() {
    let directory = tempfile::tempdir().expect("temporary directory");
    for (profile, flag) in [(Profile::Debug, "--debug"), (Profile::Release, "--release")] {
        let context = context(directory.path(), Platform::MacOs, profile);
        let options = parse(&[flag]).expect("arguments").expect("options");
        let formats = options.formats.select(context.platform).expect("formats");
        let commands = build_commands(&context, options, &formats);
        let mut commands = commands.iter();
        let cargo = commands.next().expect("Cargo build");
        assert_eq!(cargo.get_program(), "cargo");
        assert_eq!(cargo.get_current_dir(), Some(context.workspace.as_path()));
        assert_eq!(args(cargo).contains(&"--release".into()), profile == Profile::Release);
        assert!(
            cargo
                .get_envs()
                .any(|(key, value)| key == "CARGO_TARGET_DIR" && value == Some(context.target.as_os_str()))
        );
        assert!(
            cargo
                .get_envs()
                .any(|(key, value)| key == "MACOSX_DEPLOYMENT_TARGET" && value == Some(OsStr::new("15.0")))
        );
        let configure = commands.next().expect("CMake configure");
        let configure_args = args(configure);
        for required in [
            context.au_source().into_os_string(),
            context.au_build().into_os_string(),
            format!("-DCMAKE_BUILD_TYPE={}", profile.cmake_name()).into(),
            "-DCMAKE_OSX_ARCHITECTURES=arm64".into(),
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0".into(),
            "-DIMAGE_CONTROL_VERSION=0.1.0".into(),
            "-DIMAGE_CONTROL_AU_BUILD_TESTS=ON".into(),
            format!("-DIMAGE_CONTROL_RUST_LIBRARY={}", context.static_library().display()).into(),
        ] {
            assert!(
                configure_args.contains(&required),
                "missing CMake argument: {required:?}"
            );
        }
        assert_eq!(
            args(commands.next().expect("CMake build")),
            vec![
                OsString::from("--build"),
                context.au_build().into_os_string(),
                OsString::from("--config"),
                OsString::from(profile.cmake_name()),
                OsString::from("--target"),
                OsString::from("ImageControlRSAU")
            ]
        );
        assert!(commands.next().is_none());
    }
}

/// VST3 and CLAP builds never invoke `CMake` on either supported platform.
#[test]
fn dynamic_only_builds_do_not_plan_cmake() {
    let directory = tempfile::tempdir().expect("temporary directory");
    for platform in [Platform::MacOs, Platform::Windows] {
        for format in ["vst3", "clap"] {
            let context = context(directory.path(), platform, Profile::Debug);
            let options = parse(&["-f", format]).expect("arguments").expect("options");
            let commands = build_commands(&context, options, &options.formats.select(platform).expect("formats"));
            assert_eq!(commands.len(), 1);
            assert_eq!(commands.first().expect("Cargo build").get_program(), "cargo");
        }
    }
}

/// A missing requested profile cannot fall back to another profile or to a stale public bundle.
#[test]
fn missing_profile_artifacts_fail_without_touching_public_packages() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let debug = context(directory.path(), Platform::MacOs, Profile::Debug);
    let release = context(directory.path(), Platform::MacOs, Profile::Release);
    write_artifact(&debug.dynamic_library(), b"debug library");
    let old_package = release
        .bundle(PluginFormat::Clap)
        .join("Contents/MacOS/ImageControl RS");
    write_artifact(&old_package, b"previous release");
    assert!(package::validate_inputs(&release, &[PluginFormat::Clap]).is_err());
    assert_eq!(
        std::fs::read(old_package).expect("previous package"),
        b"previous release"
    );
    assert!(package::validate_inputs(&debug, &[PluginFormat::Clap]).is_ok());
    write_artifact(&release.dynamic_library(), b"");
    assert!(package::validate_inputs(&release, &[PluginFormat::Clap]).is_err());
}

/// macOS VST3 and CLAP package identical library bytes with stable, distinct bundle identities.
#[test]
fn macos_dynamic_packages_use_shared_library_and_clean_metadata() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Debug);
    write_artifact(&context.dynamic_library(), b"shared cdylib with both entry points");
    let formats = [PluginFormat::Vst3, PluginFormat::Clap];
    package::validate_inputs(&context, &formats).expect("source artifacts");
    for format in formats {
        let stale = context.bundle(format).join("Contents/_CodeSignature/CodeResources");
        write_artifact(&stale, b"stale signature");
        let bundle = package::package(&context, format).expect("package dynamic library");
        assert_eq!(
            std::fs::read(context.executable(&bundle, format).expect("executable")).expect("packaged executable"),
            b"shared cdylib with both entry points"
        );
        assert!(!stale.exists());
        let plist = std::fs::read_to_string(bundle.join("Contents/Info.plist")).expect("Info.plist");
        let identifier = if format == PluginFormat::Clap {
            "com.esgrove.imagecontrol-rs-own.clap"
        } else {
            "com.esgrove.imagecontrol-rs-own"
        };
        assert!(plist.contains(&format!("<key>CFBundleIdentifier</key><string>{identifier}</string>")));
        assert!(plist.contains("<key>CFBundleExecutable</key><string>ImageControl RS</string>"));
        assert!(plist.contains("<key>CFBundleVersion</key><string>0.1.0</string>"));
    }
}

/// A Windows CLAP package is an ordinary file, including when replacing an obsolete directory layout.
#[test]
fn windows_clap_is_a_single_file() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::Windows, Profile::Release);
    write_artifact(&context.dynamic_library(), b"Windows cdylib");
    write_artifact(&context.bundle(PluginFormat::Clap).join("obsolete layout"), b"obsolete");
    package::validate_inputs(&context, &[PluginFormat::Clap]).expect("source artifacts");
    let bundle = package::package(&context, PluginFormat::Clap).expect("CLAP package");
    assert!(bundle.is_file());
    assert_eq!(std::fs::read(bundle).expect("CLAP bytes"), b"Windows cdylib");
}

/// Audio Unit selection uses only the static archive and matching native wrapper, not the cdylib.
#[test]
fn audio_unit_copies_complete_native_bundle_without_signature_resources() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Release);
    write_audio_unit(&context);
    let source = context.au_bundle();
    write_artifact(&source.join("Contents/Resources/custom resource"), b"AU resource");
    write_artifact(
        &source.join("Contents/_CodeSignature/CodeResources"),
        b"build-tree signature",
    );
    assert!(!context.dynamic_library().exists());
    package::validate_inputs(&context, &[PluginFormat::AudioUnit]).expect("AU artifacts");
    let bundle = package::package(&context, PluginFormat::AudioUnit).expect("AU package");
    assert_eq!(
        std::fs::read(bundle.join("Contents/Info.plist")).expect("AU metadata"),
        b"native AU metadata"
    );
    assert_eq!(
        std::fs::read(bundle.join("Contents/Resources/custom resource")).expect("AU resource"),
        b"AU resource"
    );
    assert!(!bundle.join("Contents/_CodeSignature").exists());
}

/// The AU cache cannot redirect a no-build invocation to another profile, version, architecture, or archive.
#[test]
fn stale_au_configuration_is_rejected() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Release);
    write_audio_unit(&context);
    let cache_path = context.au_build().join("CMakeCache.txt");
    let original = std::fs::read_to_string(&cache_path).expect("CMake cache");
    for (valid, stale) in [
        ("CMAKE_BUILD_TYPE:STRING=Release", "CMAKE_BUILD_TYPE:STRING=Debug"),
        (
            "CMAKE_OSX_ARCHITECTURES:STRING=arm64",
            "CMAKE_OSX_ARCHITECTURES:STRING=x86_64",
        ),
        (
            "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=15.0",
            "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=14.0",
        ),
        (
            "IMAGE_CONTROL_VERSION:STRING=0.1.0",
            "IMAGE_CONTROL_VERSION:STRING=9.9.9",
        ),
        (
            "release/libimage_control_plugin_rs.a",
            "debug/libimage_control_plugin_rs.a",
        ),
    ] {
        std::fs::write(&cache_path, original.replace(valid, stale)).expect("stale CMake cache");
        assert!(
            package::validate_inputs(&context, &[PluginFormat::AudioUnit]).is_err(),
            "accepted stale cache: {stale}"
        );
    }
}

/// A recently compiled static archive requires rebuilding its wrapper before it can be packaged.
#[test]
fn au_older_than_its_archive_is_rejected() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Debug);
    write_audio_unit(&context);
    let executable = context
        .executable(&context.au_bundle(), PluginFormat::AudioUnit)
        .expect("AU executable");
    let old = SystemTime::now()
        .checked_sub(Duration::from_secs(240))
        .expect("old timestamp");
    std::fs::File::options()
        .write(true)
        .open(executable)
        .expect("AU executable")
        .set_times(std::fs::FileTimes::new().set_modified(old))
        .expect("old AU timestamp");
    assert!(package::validate_inputs(&context, &[PluginFormat::AudioUnit]).is_err());
}

/// A release AU invocation never searches a complete debug AU build or stale bundled component.
#[test]
fn au_missing_profile_does_not_fall_back_to_debug_or_bundled_component() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let debug = context(directory.path(), Platform::MacOs, Profile::Debug);
    write_audio_unit(&debug);
    let release = context(directory.path(), Platform::MacOs, Profile::Release);
    assert_ne!(debug.au_build(), release.au_build());
    write_artifact(&release.static_library(), b"release archive");
    write_artifact(
        &release.bundle(PluginFormat::AudioUnit).join("Contents/Info.plist"),
        b"stale packaged AU",
    );
    assert!(package::validate_inputs(&release, &[PluginFormat::AudioUnit]).is_err());
    assert!(package::validate_inputs(&debug, &[PluginFormat::AudioUnit]).is_ok());
}

/// Failure to copy an invalid native bundle leaves the previously published package untouched.
#[test]
fn failed_staging_does_not_replace_existing_package() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let context = context(directory.path(), Platform::MacOs, Profile::Debug);
    let old = context.bundle(PluginFormat::AudioUnit).join("previous contents");
    write_artifact(&old, b"previous package");
    assert!(package::package(&context, PluginFormat::AudioUnit).is_err());
    assert_eq!(std::fs::read(old).expect("previous package"), b"previous package");
}
