//! Shell completion scripts for `audiobatch-rs`.
//!
//! Prints the completion script that clap generates for a shell,
//! or installs it into the per-user directory that the shell loads completions from.

use std::io::Write;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use clap::Command;
use clap_complete::Shell;

/// Writes the completion script for the given shell to `output`, or installs it when `install` is set.
///
/// # Errors
///
/// Returns an error when the home directory cannot be determined,
/// when the completion directory cannot be created, or when the script cannot be written.
pub fn generate_shell_completion(
    shell: Shell,
    mut command: Command,
    install: bool,
    verbose: bool,
    command_name: &str,
    output: &mut impl Write,
) -> Result<()> {
    if install {
        let home = dirs::home_dir().context("Failed to get the home directory")?;
        let directory = shell_completion_directory(&home, shell, command_name)?;
        let path = clap_complete::generate_to(shell, &mut command, command_name, &directory)
            .with_context(|| format!("Failed to write the completion script to {}", directory.display()))?;
        if verbose {
            writeln!(output, "Completion file generated to: {}", path.display())?;
        }
    } else {
        clap_complete::generate(shell, &mut command, command_name, output);
    }
    Ok(())
}

/// Returns the completion directory for the shell under the given home directory, creating it when missing.
///
/// With oh-my-zsh installed, zsh completions go into a custom plugin named after the command,
/// which is then enabled in the plugins list of `.zshrc`.
/// The system wide directories are not used, because writing them needs administrator rights.
fn shell_completion_directory(home: &Path, shell: Shell, command_name: &str) -> Result<PathBuf> {
    let oh_my_zsh_plugins = home.join(".oh-my-zsh/custom/plugins");
    let directory = match shell {
        Shell::Zsh if oh_my_zsh_plugins.is_dir() => oh_my_zsh_plugins.join(command_name),
        Shell::Zsh => home.join(".zsh/completions"),
        Shell::Bash => home.join(".bash_completion.d"),
        Shell::Fish => home.join(".config/fish/completions"),
        Shell::Elvish => home.join(".elvish"),
        Shell::PowerShell if cfg!(windows) => home.join(r"Documents\PowerShell\completions"),
        Shell::PowerShell => home.join(".config/powershell/completions"),
        _ => anyhow::bail!("Unsupported shell: {shell}"),
    };
    std::fs::create_dir_all(&directory)
        .with_context(|| format!("Failed to create the completion directory {}", directory.display()))?;
    Ok(directory)
}

#[cfg(test)]
mod test_shell_completion_directory {
    use super::*;
    use tempfile::TempDir;

    /// Returns a temporary home directory that stands in for the user's own.
    fn home() -> TempDir {
        TempDir::new().expect("temporary directory")
    }

    #[test]
    fn oh_my_zsh_gets_a_custom_plugin_directory() {
        let home = home();
        std::fs::create_dir_all(home.path().join(".oh-my-zsh/custom/plugins")).expect("plugins directory");

        let directory = shell_completion_directory(home.path(), Shell::Zsh, "audiobatch-rs").expect("directory");

        assert_eq!(directory, home.path().join(".oh-my-zsh/custom/plugins/audiobatch-rs"));
        assert!(directory.is_dir());
    }

    #[test]
    fn zsh_without_oh_my_zsh_uses_the_user_directory() {
        let home = home();

        let directory = shell_completion_directory(home.path(), Shell::Zsh, "audiobatch-rs").expect("directory");

        assert_eq!(directory, home.path().join(".zsh/completions"));
        assert!(directory.is_dir());
    }

    #[test]
    fn oh_my_zsh_only_applies_to_zsh() {
        let home = home();
        std::fs::create_dir_all(home.path().join(".oh-my-zsh/custom/plugins")).expect("plugins directory");

        let directory = shell_completion_directory(home.path(), Shell::Bash, "audiobatch-rs").expect("directory");

        assert_eq!(directory, home.path().join(".bash_completion.d"));
    }

    #[test]
    fn powershell_directory_depends_on_the_platform() {
        let home = home();
        let expected = if cfg!(windows) {
            home.path().join(r"Documents\PowerShell\completions")
        } else {
            home.path().join(".config/powershell/completions")
        };

        let directory = shell_completion_directory(home.path(), Shell::PowerShell, "audiobatch-rs").expect("directory");

        assert_eq!(directory, expected);
        assert!(directory.is_dir());
    }
}

#[cfg(test)]
mod test_generate_shell_completion {
    use super::*;
    use crate::args::CliArgs;
    use clap::CommandFactory;

    #[test]
    fn prints_the_script_without_install() {
        let mut output = Vec::new();

        generate_shell_completion(
            Shell::Zsh,
            CliArgs::command(),
            false,
            true,
            "audiobatch-rs",
            &mut output,
        )
        .expect("generates");

        let script = String::from_utf8(output).expect("utf-8");
        assert!(script.contains("#compdef audiobatch-rs"));
        assert!(script.contains("--recurse"));
    }

    #[test]
    fn every_supported_shell_generates_a_script() {
        for shell in [Shell::Bash, Shell::Elvish, Shell::Fish, Shell::PowerShell, Shell::Zsh] {
            let mut output = Vec::new();
            generate_shell_completion(shell, CliArgs::command(), false, false, "audiobatch-rs", &mut output)
                .expect("generates");
            assert!(!output.is_empty(), "{shell} script is empty");
        }
    }
}
