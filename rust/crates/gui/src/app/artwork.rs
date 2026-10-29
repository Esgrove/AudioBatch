//! The embedded album artwork of the selected file, shown beside its name in the information panel.
//!
//! A new selection reads the artwork on a background thread, independent of the tag read,
//! so a slow or huge picture never delays the cue points or the Serato Overview.
//! Each read replaces the task of the previous one, and a result is shown only while its file is still selected,
//! unchanged since the read started, and the option is on, so a stale read can never show the wrong cover.
//! Turning the option off drops the thumbnail and its texture, and turning it on reads the selected file again.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use audiobatch_core::record::file_identity;
use gpui_kit::{Context, RenderImage, Task};

use super::AppView;
use crate::artwork::load_artwork_thumbnail;

impl AppView {
    /// Starts reading the artwork of a newly selected file, replacing any read in progress.
    /// Clears the artwork of the previous file at once, so it never shows beside another name.
    pub(super) fn request_artwork(&mut self, path: Option<&Path>, cx: &mut Context<Self>) {
        self.set_artwork(None, cx);
        self.artwork_task = path
            .filter(|_| self.settings.show_album_artwork)
            .map(|path| Self::spawn_artwork_read(path.to_path_buf(), cx));
    }

    /// Returns the thumbnail of the selected file, when it has one and the option is on.
    pub(super) fn selected_artwork(&self) -> Option<Arc<RenderImage>> {
        self.selected_artwork
            .as_ref()
            .filter(|(path, _)| self.settings.show_album_artwork && self.selected.as_ref() == Some(path))
            .map(|(_, image)| Arc::clone(image))
    }

    /// Reads the artwork of the selected file again after the option was turned on, or drops it when turned off.
    pub(super) fn refresh_artwork(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected.clone();
        self.request_artwork(selected.as_deref(), cx);
    }

    /// Reads and decodes the artwork of a file on a background thread,
    /// and shows it when the file is still selected and unchanged.
    fn spawn_artwork_read(path: PathBuf, cx: &Context<Self>) -> Task<()> {
        cx.spawn(async move |this, cx| {
            let read_path = path.clone();
            let (identity, thumbnail) = cx
                .background_executor()
                .spawn(async move {
                    let identity = file_identity(&read_path);
                    (identity, load_artwork_thumbnail(&read_path))
                })
                .await;
            let _ = this.update(cx, |view, cx| {
                let current = view.settings.show_album_artwork
                    && view.selected.as_deref() == Some(path.as_path())
                    && identity.is_some()
                    && file_identity(&path) == identity;
                if current {
                    view.set_artwork(thumbnail.map(|image| (path, Arc::new(image))), cx);
                }
            });
        })
    }

    /// Replaces the shown artwork and frees the texture of the previous thumbnail.
    /// The texture is freed after the current update, when no window is borrowed,
    /// so it leaves the sprite atlas of every window.
    fn set_artwork(&mut self, artwork: Option<(PathBuf, Arc<RenderImage>)>, cx: &mut Context<Self>) {
        let previous = std::mem::replace(&mut self.selected_artwork, artwork);
        if let Some((_, image)) = previous {
            cx.defer(move |cx| cx.drop_image(image, None));
        }
        cx.notify();
    }
}
