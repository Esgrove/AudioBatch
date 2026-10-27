//! The plugin listing of `audiobatch-rs list`: the optional rescan, and the table and grouped output.
//!
//! The listing goes to stdout, and the scan messages, warnings, and the plugin count go to stderr,
//! so the listing can be piped without the scan output mixed in.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::io::Write;
use std::path::Path;

use anyhow::{Context as _, Result};
use audiobatch_core::sorting::compare_natural;
use audiobatch_plugins::{
    FORMAT_NAME, KnownPlugins, PluginDataPaths, PluginDescription, SCAN_BUSY_MESSAGE, SCAN_TIMEOUT, ScanLock,
    ScanOptions, ScanReport, WorkerCommand, scan_plugins,
};
use colored::Colorize;
use unicode_width::UnicodeWidthStr;

use crate::args::{PluginGrouping, PluginListing};
use crate::run::{EXIT_SUCCESS, EXIT_USAGE_ERROR};

/// Label for a plugin without a manufacturer, and the group sorted last.
const UNKNOWN_LABEL: &str = "Unknown";

/// Indent of the rows under a group header.
const GROUP_INDENT: &str = "  ";

/// One column of the plugin listing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Column {
    /// The plugin name.
    Name,
    /// The manufacturer, or "Unknown".
    Manufacturer,
    /// The processing type.
    Type,
    /// The plugin format.
    Format,
    /// The plugin version.
    Version,
}

/// Where the listing reads and saves the known plugins, and how a rescan starts its worker.
#[derive(Debug, Clone)]
pub struct PluginListContext {
    /// The known plugins file, the scan lock, and the worker log.
    pub data_paths: PluginDataPaths,
    /// How to start the scan worker.
    pub worker: WorkerCommand,
    /// Whether to show the scan progress line, which only makes sense on a terminal.
    pub show_progress: bool,
}

/// The plugins of one group, with the label as first seen.
struct Group<'a> {
    /// The label shown in the header.
    label: String,
    /// The plugins in listing order.
    plugins: Vec<&'a PluginDescription>,
}

impl Column {
    /// Every column in listing order.
    const ALL: [Self; 5] = [Self::Name, Self::Manufacturer, Self::Type, Self::Format, Self::Version];

    /// Returns the header text.
    const fn header(self) -> &'static str {
        match self {
            Self::Name => "NAME",
            Self::Manufacturer => "MANUFACTURER",
            Self::Type => "TYPE",
            Self::Format => "FORMAT",
            Self::Version => "VERSION",
        }
    }

    /// Returns the text shown for a plugin.
    fn value(self, plugin: &PluginDescription) -> String {
        match self {
            Self::Name => plugin.name.clone(),
            Self::Manufacturer => manufacturer_label(plugin),
            Self::Type => type_label(plugin),
            Self::Format => String::from(FORMAT_NAME),
            Self::Version => plugin.version.clone(),
        }
    }
}

/// Prints the known plugins arranged as requested, after a rescan when asked, and returns the exit code.
///
/// The scan lock is held from loading the list to saving it,
/// so a scan in the GUI cannot overwrite the results of this one, or the other way around.
///
/// # Errors
///
/// Returns an error when the known plugins cannot be read, the scan cannot run, or the output cannot be written.
pub fn run_plugin_list(
    listing: PluginListing,
    context: &PluginListContext,
    output: &mut dyn Write,
    log: &mut dyn Write,
) -> Result<u8> {
    let paths = &context.data_paths;
    let lock = if listing.rescan {
        let Some(lock) = ScanLock::try_acquire(&paths.scan_lock)? else {
            writeln!(log, "{}", format!("Error: {SCAN_BUSY_MESSAGE}").red())?;
            return Ok(EXIT_USAGE_ERROR);
        };
        Some(lock)
    } else {
        None
    };
    let mut known = KnownPlugins::load_from(&paths.known_plugins)
        .with_context(|| format!("Could not read the plugin list {}", paths.known_plugins.display()))?;

    let mut exit_code = EXIT_SUCCESS;
    if let Some(lock) = &lock {
        rescan(lock, &mut known, context, log)?;
        if known.save_to(&paths.known_plugins).is_err() {
            let message = format!(
                "Error: Could not save the plugin list to {}",
                paths.known_plugins.display()
            );
            writeln!(log, "{}", message.red())?;
            exit_code = EXIT_USAGE_ERROR;
        }
    }
    drop(lock);

    if known.plugins.is_empty() {
        let message = "Warning: No plugins found. Run with --rescan, or scan for plugins in the GUI.";
        writeln!(log, "{}", message.yellow())?;
        return Ok(exit_code);
    }
    let mut plugins = known.plugins;
    sort_plugins(&mut plugins);
    output.write_all(format_plugin_list(&plugins, listing.grouping).as_bytes())?;
    output.flush()?;
    writeln!(log, "{}", format!("{} plugins", plugins.len()).dimmed())?;
    Ok(exit_code)
}

/// Returns the listing text: one table for the name grouping, otherwise one section per group.
#[must_use]
pub fn format_plugin_list(plugins: &[PluginDescription], grouping: PluginGrouping) -> String {
    grouped_column(grouping).map_or_else(
        || format_table(plugins),
        |grouped| format_groups(plugins, grouping, grouped),
    )
}

/// Sorts plugins by name, then manufacturer, then format, in natural order ignoring case.
pub fn sort_plugins(plugins: &mut [PluginDescription]) {
    plugins.sort_by(|first, second| {
        compare_natural(&first.name, &second.name).then_with(|| compare_natural(&first.vendor, &second.vendor))
    });
}

/// Scans the plugin folders for new or changed plugins and reports the bundles that could not be scanned.
fn rescan(lock: &ScanLock, known: &mut KnownPlugins, context: &PluginListContext, log: &mut dyn Write) -> Result<()> {
    let folders = known.search_folders();
    let folder_list = folders
        .iter()
        .map(|folder| folder.display().to_string())
        .collect::<Vec<_>>()
        .join(";");
    writeln!(
        log,
        "{}",
        format!("Scanning {FORMAT_NAME} plugins in {folder_list}").bold()
    )?;
    let options = ScanOptions {
        folders,
        worker: context.worker.clone(),
        worker_log: Some(context.data_paths.worker_log.clone()),
        timeout: SCAN_TIMEOUT,
    };
    let show_progress = context.show_progress;
    let report = scan_plugins(lock, known, &options, &|| false, &mut |progress| {
        if show_progress {
            let percent = progress.fraction * 100.0;
            let _ = write!(log, "\r\x1b[K{percent:3.0}%  {}", file_name(progress.bundle));
            let _ = log.flush();
        }
    })?;
    if show_progress {
        write!(log, "\r\x1b[K")?;
    }
    report_scan_problems(&report, log)
}

/// Prints a warning for each bundle that crashed, took too long, or contained no plugins.
fn report_scan_problems(report: &ScanReport, log: &mut dyn Write) -> Result<()> {
    let warnings = report
        .crashed
        .iter()
        .map(|bundle| {
            format!(
                "Plugin crashed during the scan and was blacklisted: {}",
                bundle.display()
            )
        })
        .chain(report.timed_out.iter().map(|bundle| {
            format!(
                "Plugin took longer than {} seconds to scan and was skipped: {}",
                SCAN_TIMEOUT.as_secs(),
                bundle.display()
            )
        }))
        .chain(
            report
                .no_plugins
                .iter()
                .map(|bundle| format!("No plugins found in {}", bundle.display())),
        );
    for warning in warnings {
        writeln!(log, "{}", format!("Warning: {warning}").yellow())?;
    }
    Ok(())
}

/// Returns the file name of a bundle, or the whole path when it has none.
fn file_name(bundle: &Path) -> String {
    bundle.file_name().map_or_else(
        || bundle.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

/// Returns the manufacturer of a plugin, or "Unknown".
fn manufacturer_label(plugin: &PluginDescription) -> String {
    if plugin.vendor.is_empty() {
        String::from(UNKNOWN_LABEL)
    } else {
        plugin.vendor.clone()
    }
}

/// Returns the processing type of a plugin, such as Dynamics, EQ, or Reverb.
///
/// VST3 categories are lists like "Fx|Dynamics".
/// The generic Fx and Instrument entries are skipped in favour of the first specific one.
fn type_label(plugin: &PluginDescription) -> String {
    plugin
        .category
        .split('|')
        .map(str::trim)
        .find(|token| {
            !token.is_empty() && !token.eq_ignore_ascii_case("Fx") && !token.eq_ignore_ascii_case("Instrument")
        })
        .map_or_else(
            || String::from(if plugin.is_instrument { "Instrument" } else { "Effect" }),
            String::from,
        )
}

/// Returns the column a grouping shows in its group headers, or `None` for the alphabetical table.
const fn grouped_column(grouping: PluginGrouping) -> Option<Column> {
    match grouping {
        PluginGrouping::Name => None,
        PluginGrouping::Manufacturer => Some(Column::Manufacturer),
        PluginGrouping::Type => Some(Column::Type),
        PluginGrouping::Format => Some(Column::Format),
    }
}

/// Returns the display width of each column, from every value and optionally the header,
/// so rows line up across all groups.
fn column_widths(plugins: &[&PluginDescription], columns: &[Column], include_headers: bool) -> Vec<usize> {
    columns
        .iter()
        .map(|column| {
            let header_width = if include_headers { column.header().width() } else { 0 };
            plugins
                .iter()
                .map(|plugin| column.value(plugin).width())
                .fold(header_width, usize::max)
        })
        .collect()
}

/// Returns one listing line from the cells.
/// Every cell except the last is padded to its column width, so lines do not end in spaces.
fn format_line(indent: &str, cells: &[String], widths: &[usize]) -> String {
    let mut line = String::from(indent);
    for (index, (cell, width)) in cells.iter().zip(widths).enumerate() {
        line.push_str(cell);
        if index + 1 < cells.len() {
            line.push_str(&" ".repeat(width.saturating_sub(cell.width()) + 2));
        }
    }
    line
}

/// Appends one aligned row per plugin.
fn push_rows(text: &mut String, plugins: &[&PluginDescription], columns: &[Column], widths: &[usize], indent: &str) {
    for plugin in plugins {
        let cells: Vec<String> = columns.iter().map(|column| column.value(plugin)).collect();
        let _ = writeln!(text, "{}", format_line(indent, &cells, widths));
    }
}

/// Returns every plugin as one table with a bold header line.
fn format_table(plugins: &[PluginDescription]) -> String {
    let plugins: Vec<&PluginDescription> = plugins.iter().collect();
    let widths = column_widths(&plugins, &Column::ALL, true);
    let headers: Vec<String> = Column::ALL.iter().map(|column| String::from(column.header())).collect();
    let mut text = format!("{}\n", format_line("", &headers, &widths).bold());
    push_rows(&mut text, &plugins, &Column::ALL, &widths, "");
    text
}

/// Returns one section per group, sorted by label with "Unknown" last.
/// Labels are matched ignoring case, so differently capitalized manufacturer names end up together.
fn format_groups(plugins: &[PluginDescription], grouping: PluginGrouping, grouped: Column) -> String {
    let mut groups_by_key: HashMap<String, Group<'_>> = HashMap::new();
    for plugin in plugins {
        let label = group_label(plugin, grouping);
        groups_by_key
            .entry(label.to_lowercase())
            .or_insert_with(|| Group {
                label,
                plugins: Vec::new(),
            })
            .plugins
            .push(plugin);
    }
    let mut groups: Vec<Group<'_>> = groups_by_key.into_values().collect();
    groups.sort_by(|first, second| {
        let first_unknown = first.label.eq_ignore_ascii_case(UNKNOWN_LABEL);
        let second_unknown = second.label.eq_ignore_ascii_case(UNKNOWN_LABEL);
        first_unknown
            .cmp(&second_unknown)
            .then_with(|| compare_natural(&first.label, &second.label))
    });

    let columns: Vec<Column> = Column::ALL.into_iter().filter(|column| *column != grouped).collect();
    let all_plugins: Vec<&PluginDescription> = plugins.iter().collect();
    let widths = column_widths(&all_plugins, &columns, false);
    let mut text = String::new();
    for (index, group) in groups.iter().enumerate() {
        if index > 0 {
            text.push('\n');
        }
        let _ = writeln!(text, "{} ({})", group.label.cyan().bold(), group.plugins.len());
        push_rows(&mut text, &group.plugins, &columns, &widths, GROUP_INDENT);
    }
    text
}

/// Returns the group of a plugin for a grouped listing.
fn group_label(plugin: &PluginDescription, grouping: PluginGrouping) -> String {
    match grouping {
        PluginGrouping::Manufacturer => manufacturer_label(plugin),
        PluginGrouping::Type => type_label(plugin),
        PluginGrouping::Format => String::from(FORMAT_NAME),
        PluginGrouping::Name => String::new(),
    }
}

#[cfg(test)]
mod test_plugin_list_format {
    use std::path::PathBuf;

    use audiobatch_plugins::PluginDescription;
    use vst3_host::ClassId;

    use super::*;

    /// Returns a plugin with the given name, manufacturer, and category.
    fn plugin(name: &str, vendor: &str, category: &str) -> PluginDescription {
        PluginDescription {
            name: String::from(name),
            vendor: String::from(vendor),
            version: String::from("1.0.0"),
            category: String::from(category),
            class_id: ClassId([0; 16]),
            sdk_version: String::from("VST 3.8.0"),
            bundle: PathBuf::from("/Plugins/Test.vst3"),
            bundle_modified_ms: 0,
            is_instrument: false,
            main_input_channels: Some(2),
            main_output_channels: Some(2),
        }
    }

    /// Removes the terminal styling escapes from the text.
    fn plain(text: &str) -> String {
        let mut result = String::new();
        let mut characters = text.chars();
        while let Some(character) = characters.next() {
            if character == '\x1b' {
                characters.by_ref().find(|next| *next == 'm');
            } else {
                result.push(character);
            }
        }
        result
    }

    /// Returns the listing of the plugins after sorting them.
    fn listing(mut plugins: Vec<PluginDescription>, grouping: PluginGrouping) -> String {
        sort_plugins(&mut plugins);
        plain(&format_plugin_list(&plugins, grouping))
    }

    #[test]
    fn type_is_the_first_specific_category() {
        assert_eq!(type_label(&plugin("A", "V", "Fx|Dynamics")), "Dynamics");
        assert_eq!(type_label(&plugin("A", "V", " fx | |EQ|Filter")), "EQ");
        assert_eq!(type_label(&plugin("A", "V", "Instrument|Synth")), "Synth");
        assert_eq!(type_label(&plugin("A", "V", "Fx")), "Effect");
        let instrument = PluginDescription {
            is_instrument: true,
            ..plugin("A", "V", "Instrument")
        };
        assert_eq!(type_label(&instrument), "Instrument");
    }

    #[test]
    fn table_is_aligned_and_sorted_naturally() {
        let text = listing(
            vec![
                plugin("Comp 10", "Studio", "Fx|Dynamics"),
                plugin("Comp 2", "", "Fx|Dynamics"),
                plugin("äEQ", "Studio", "Fx|EQ"),
            ],
            PluginGrouping::Name,
        );
        let expected = "\
NAME     MANUFACTURER  TYPE      FORMAT  VERSION
Comp 2   Unknown       Dynamics  VST3    1.0.0
Comp 10  Studio        Dynamics  VST3    1.0.0
äEQ      Studio        EQ        VST3    1.0.0
";
        assert_eq!(text, expected);
    }

    #[test]
    fn manufacturer_groups_merge_case_and_put_unknown_last() {
        let text = listing(
            vec![
                plugin("Reverb", "", "Fx|Reverb"),
                plugin("Delay", "studio", "Fx|Delay"),
                plugin("Comp", "Studio", "Fx|Dynamics"),
                plugin("Gate", "Acme", "Fx|Dynamics"),
            ],
            PluginGrouping::Manufacturer,
        );
        let expected = "\
Acme (1)
  Gate    Dynamics  VST3  1.0.0

Studio (2)
  Comp    Dynamics  VST3  1.0.0
  Delay   Delay     VST3  1.0.0

Unknown (1)
  Reverb  Reverb    VST3  1.0.0
";
        assert_eq!(text, expected);
    }

    #[test]
    fn type_and_format_groups_leave_out_their_column() {
        let plugins = vec![plugin("Comp", "Studio", "Fx|Dynamics"), plugin("EQ", "Acme", "Fx|EQ")];
        let by_type = listing(plugins.clone(), PluginGrouping::Type);
        assert_eq!(
            by_type,
            "Dynamics (1)\n  Comp  Studio  VST3  1.0.0\n\nEQ (1)\n  EQ    Acme    VST3  1.0.0\n"
        );
        let by_format = listing(plugins, PluginGrouping::Format);
        assert_eq!(
            by_format,
            "VST3 (2)\n  Comp  Studio  Dynamics  1.0.0\n  EQ    Acme    EQ        1.0.0\n"
        );
    }
}

#[cfg(test)]
mod test_run_plugin_list {
    use std::ffi::OsString;
    use std::path::PathBuf;

    use audiobatch_plugins::WORKER_FLAG;
    use tempfile::TempDir;

    use super::*;

    /// A private data folder for the known plugins and the scan files.
    struct Setup {
        /// The data folder.
        directory: TempDir,
        /// The listing context using it.
        context: PluginListContext,
    }

    impl Setup {
        /// Creates an empty data folder.
        fn new() -> Self {
            let directory = tempfile::tempdir().expect("temporary folder");
            let context = PluginListContext {
                data_paths: PluginDataPaths::in_folder(directory.path()),
                worker: WorkerCommand {
                    program: PathBuf::from("audiobatch-missing-worker"),
                    arguments: vec![OsString::from(WORKER_FLAG)],
                    environment: Vec::new(),
                },
                show_progress: false,
            };
            Self { directory, context }
        }

        /// Runs the listing and returns the exit code, stdout, and stderr.
        fn run(&self, rescan: bool) -> (u8, String, String) {
            let mut output = Vec::new();
            let mut log = Vec::new();
            let listing = PluginListing {
                rescan,
                grouping: PluginGrouping::Name,
            };
            let code = run_plugin_list(listing, &self.context, &mut output, &mut log).expect("listing runs");
            (
                code,
                String::from_utf8(output).expect("utf-8"),
                String::from_utf8(log).expect("utf-8"),
            )
        }
    }

    #[test]
    fn saved_plugins_are_listed_with_a_count() {
        let setup = Setup::new();
        let plugin = PluginDescription {
            name: String::from("Saved"),
            vendor: String::from("Studio"),
            version: String::from("2.1"),
            category: String::from("Fx|EQ"),
            class_id: vst3_host::ClassId([0; 16]),
            sdk_version: String::new(),
            bundle: PathBuf::from("/Plugins/Saved.vst3"),
            bundle_modified_ms: 0,
            is_instrument: false,
            main_input_channels: None,
            main_output_channels: None,
        };
        let known = KnownPlugins {
            plugins: vec![plugin],
            ..KnownPlugins::default()
        };
        known.save_to(&setup.context.data_paths.known_plugins).expect("saved");
        let (code, output, log) = setup.run(false);
        assert_eq!(code, EXIT_SUCCESS);
        assert!(output.contains("Saved  Studio        EQ    VST3    2.1"), "{output}");
        assert!(log.contains("1 plugins"), "{log}");
    }

    #[test]
    fn an_empty_list_suggests_a_rescan() {
        let (code, output, log) = Setup::new().run(false);
        assert_eq!(code, EXIT_SUCCESS);
        assert_eq!(output, "");
        assert!(log.contains("No plugins found. Run with --rescan, or scan for plugins in the GUI."));
    }

    #[test]
    fn a_rescan_saves_the_list_and_scans_the_custom_folders() {
        let setup = Setup::new();
        let folder = setup.directory.path().join("empty plugins");
        std::fs::create_dir_all(&folder).expect("plugin folder");
        let known = KnownPlugins {
            custom_folders: vec![folder.clone()],
            ..KnownPlugins::default()
        };
        known.save_to(&setup.context.data_paths.known_plugins).expect("saved");
        let (code, _, log) = setup.run(true);
        assert_eq!(code, EXIT_SUCCESS);
        assert!(
            log.contains(&format!("Scanning VST3 plugins in {}", folder.display())),
            "{log}"
        );
        let saved = KnownPlugins::load_from(&setup.context.data_paths.known_plugins).expect("loads");
        assert_eq!(saved.custom_folders, vec![folder]);
    }

    #[test]
    fn a_rescan_refuses_to_run_during_another_scan() {
        let setup = Setup::new();
        let _held = ScanLock::try_acquire(&setup.context.data_paths.scan_lock)
            .expect("lock file")
            .expect("free lock");
        let (code, _, log) = setup.run(true);
        assert_eq!(code, EXIT_USAGE_ERROR);
        assert!(log.contains(SCAN_BUSY_MESSAGE), "{log}");
        let (code, _, _) = setup.run(false);
        assert_eq!(code, EXIT_SUCCESS);
    }

    #[test]
    fn an_unreadable_list_is_an_error() {
        let setup = Setup::new();
        std::fs::write(&setup.context.data_paths.known_plugins, "not json").expect("written");
        let listing = PluginListing::default();
        let result = run_plugin_list(listing, &setup.context, &mut Vec::new(), &mut Vec::new());
        assert!(result.is_err());
    }
}
