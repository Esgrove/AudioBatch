//! Retained preset callout and detached worker completions, independent of the audio callback.

use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver};

use crate::editing::SoundSnapshot;
use crate::user_presets::{PresetLibrary, UserPreset};

#[cfg(test)]
#[path = "preset_editor_tests.rs"]
mod tests;

/// Explicit local actions never run from a host state callback or audio processing.
#[derive(Clone, Copy, Debug)]
pub enum Action {
    Refresh,
    Load,
    Save,
    SaveAs,
    Rename,
    Delete,
    Import,
    Export,
    Reconnect,
}

/// One filesystem completion carries the editor and sound selection identities checked before applying it.
pub(super) struct Completion {
    pub(super) generation: u64,
    pub(super) epoch: u64,
    pub(super) slot: usize,
    pub(super) before: SoundSnapshot,
    pub(super) result: Result<Outcome, String>,
}

/// Metadata-only results do not accidentally recall sound or replace a project baseline on reconnect.
pub(super) enum Outcome {
    Listed(Vec<UserPreset>),
    Loaded(UserPreset),
    Saved(UserPreset),
    Renamed(UserPreset),
    Reconnected(UserPreset),
    Imported,
    Deleted,
    Exported,
}

/// UI-owned transient library bindings never enter saved projects.
#[derive(Default)]
pub(super) struct PresetEditor {
    pub(super) shown: bool,
    pub(super) name: String,
    pub(super) path: String,
    pub(super) entries: Vec<UserPreset>,
    pub(super) bound: [Option<UserPreset>; 2],
    pub(super) error: Option<String>,
    pub(super) confirmation: Option<Action>,
    binding: Option<std::rc::Rc<dyn crate::params::EditorBinding>>,
    pub(super) epoch: u64,
    receiver: Option<Receiver<Completion>>,
    worker: crate::params::EditorShutdown,
}

impl Drop for PresetEditor {
    /// Retires file work before the editor's code can unload. No task depends on a later UI callback.
    fn drop(&mut self) {
        self.shutdown().shutdown();
    }
}

impl PresetEditor {
    /// Attaches independent worker retirement to the native view's close boundary.
    pub(super) fn with_binding(binding: std::rc::Rc<dyn crate::params::EditorBinding>) -> Self {
        let mut editor = Self::default();
        binding.register_shutdown(editor.shutdown());
        editor.binding = Some(binding);
        editor
    }

    /// Gives native removal independent join ownership even if Iced retains or is polling the program state.
    pub(super) fn shutdown(&self) -> crate::params::EditorShutdown {
        self.worker.clone()
    }

    /// Invalidates transient library permission after project recall without reading any file.
    pub(super) fn invalidate(&mut self) {
        self.bound = [None, None];
        self.confirmation = None;
        self.epoch = self.epoch.wrapping_add(1);
    }

    /// Reports one in-flight operation without blocking the editor's event loop.
    pub(super) const fn busy(&self) -> bool {
        self.receiver.is_some()
    }

    /// Polls worker completion without notifications, callbacks, or ownership on the audio thread.
    pub(super) fn poll(&mut self) -> Option<Completion> {
        let completion = match self.receiver.as_ref()?.try_recv() {
            Ok(completion) => completion,
            Err(mpsc::TryRecvError::Empty) => return None,
            Err(mpsc::TryRecvError::Disconnected) => {
                self.receiver = None;
                self.worker.finish();
                self.error = Some("The preset worker stopped before completing the operation".into());
                return None;
            }
        };
        self.receiver = None;
        self.worker.finish();
        Some(completion)
    }

    /// Runs filesystem work with detached values. Completion publication is checked by the editor owner.
    pub(super) fn start(
        &mut self,
        action: Action,
        generation: u64,
        slot: usize,
        sound: SoundSnapshot,
        identity: Option<String>,
    ) {
        if self.busy() || self.worker.is_closed() || self.binding.as_ref().is_some_and(|binding| !binding.is_open()) {
            return;
        }
        let epoch = self.epoch;
        let name = self.name.clone();
        let path = PathBuf::from(&self.path);
        let selection = self.bound.get(slot).cloned().flatten();
        let (sender, receiver) = mpsc::channel();
        let worker = self.worker.spawn(move || {
            let result = (|| -> Result<Outcome, String> {
                let library = PresetLibrary::new(PresetLibrary::default_path().map_err(|error| error.to_string())?);
                let selected = || {
                    selection
                        .as_ref()
                        .ok_or_else(|| "Reconnect or select a library preset first".to_owned())
                };
                match action {
                    Action::Refresh => library.list().map(Outcome::Listed),
                    Action::Load => library.reconnect(&selected()?.id).and_then(|preset| {
                        let preset = preset.ok_or_else(|| {
                            std::io::Error::new(
                                std::io::ErrorKind::NotFound,
                                "The preset disappeared. Refresh the library",
                            )
                        })?;
                        if preset.revision != selected().map_err(std::io::Error::other)?.revision {
                            return Err(std::io::Error::other("The preset changed. Refresh the library"));
                        }
                        Ok(Outcome::Loaded(preset))
                    }),
                    Action::SaveAs => library.create(&name, sound).map(Outcome::Saved),
                    Action::Save => library.update(selected()?, &name, sound).map(Outcome::Saved),
                    Action::Rename => library
                        .update(selected()?, &name, selected()?.sound)
                        .map(Outcome::Renamed),
                    Action::Delete => library.delete(selected()?).map(|()| Outcome::Deleted),
                    Action::Import => library.import(&path).map(|_| Outcome::Imported),
                    Action::Export => library.export(selected()?, &path).map(|()| Outcome::Exported),
                    Action::Reconnect => library
                        .reconnect(&identity.ok_or_else(|| "No preset identity in this slot".to_owned())?)
                        .and_then(|preset| {
                            preset.ok_or_else(|| {
                                std::io::Error::new(
                                    std::io::ErrorKind::NotFound,
                                    "The selected preset is not in the library",
                                )
                            })
                        })
                        .map(Outcome::Reconnected),
                }
                .map_err(|error| error.to_string())
            })();
            let _ = sender.send(Completion {
                generation,
                epoch,
                slot,
                before: sound,
                result,
            });
        });
        match worker {
            Ok(true) => self.receiver = Some(receiver),
            Ok(false) => {}
            Err(error) => self.error = Some(error.to_string()),
        }
    }
}
