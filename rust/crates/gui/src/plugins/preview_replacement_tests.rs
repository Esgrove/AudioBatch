//! Hardware-free ownership regressions for replacing a live CLAP preview through the production chain seam.
//!
//! Player construction requires a physical output device, so these tests retain its ordinary boxed insert
//! and preview tracker directly.
//! They exercise the main view's production retirement-before-acquisition method,
//! without replacing native instances or simulating a second processor model.

use audiobatch_playback::PreviewInsert;
use audiobatch_plugins::{HostError, PluginFormat, PluginId};
use audiobatch_test_clap_plugin::{GAIN_PARAMETER, PLUGIN_ID};
use tempfile::TempDir;
use vst3_host::UiThread;

use super::{LiveChain, SlotId};
use crate::plugins::preview::{ChainInsert, PluginPreview};
use crate::test_helpers::{effect, plugin_dispatcher};

/// Production insert ownership, preview bookkeeping, and live instances, all retained until UI-thread disposal.
struct Fixture {
    insert: Option<Box<dyn PreviewInsert>>,
    preview: PluginPreview,
    chain: LiveChain,
    _directory: TempDir,
}

impl Fixture {
    /// Loads two copies of the repository CLAP effect, each with half gain and no native editor.
    fn new(ui: &UiThread) -> Self {
        let directory = TempDir::new().expect("isolated CLAP bundles and chain");
        let bundle = audiobatch_test_clap_plugin::create_bundle(directory.path(), "PreviewReplacement")
            .expect("build the CLAP fixture with cargo build -p audiobatch-test-clap-plugin --example clap_module");
        let mut plugin = effect("AudioBatch Tests", "CLAP replacement fixture");
        plugin.format = PluginFormat::Clap;
        plugin.class_id = PluginId::Clap(PLUGIN_ID.to_str().expect("fixture id").to_owned());
        plugin.bundle = bundle;
        let mut chain = LiveChain::new(directory.path().join("plugin-chain.json"), plugin_dispatcher(), 48_000);
        for _ in 0..2 {
            let id = chain.add(ui, plugin.clone());
            let slot = chain.slots.iter().find(|slot| slot.id == id).expect("added slot");
            assert!(slot.is_ready(), "{:?}", slot.error());
            slot.instance
                .as_ref()
                .expect("loaded CLAP instance")
                .set_parameter(ui, GAIN_PARAMETER.get(), 0.5)
                .expect("plain half gain");
        }
        chain.capture_states(ui).expect("flush and save half gain");
        Self {
            insert: None,
            preview: PluginPreview::default(),
            chain,
            _directory: directory,
        }
    }

    /// Replaces the boxed production insert using the same acquisition seam as the main preview owner.
    fn replace(&mut self, ui: &UiThread) {
        let chain = self
            .chain
            .replace_preview_chain(ui, || {
                self.preview.detached();
                drop(self.insert.take());
                assert!(
                    self.chain
                        .slots
                        .iter()
                        .filter_map(|slot| slot.instance.as_ref())
                        .all(|instance| !instance.has_processor_handles()),
                    "retirement must release every existing lease before acquisition"
                );
            })
            .expect("replacement acquires each CLAP lease once")
            .expect("an enabled slot remains");
        self.preview.attached(chain.failure(), chain.tail_frames());
        self.insert = Some(Box::new(ChainInsert(chain)));
    }

    /// Processes a bounded block through the real insert without a physical device.
    fn assert_gain(&mut self, gain: f32) {
        let mut left = [0.5_f32; 17];
        let mut right = [-0.25_f32; 17];
        assert!(
            self.insert
                .as_mut()
                .expect("attached insert")
                .process(&mut left, &mut right)
        );
        let expected_left = 0.5 * gain;
        let expected_right = -0.25 * gain;
        assert!(left.iter().all(|sample| (*sample - expected_left).abs() < f32::EPSILON));
        assert!(
            right
                .iter()
                .all(|sample| (*sample - expected_right).abs() < f32::EPSILON)
        );
        assert!(self.preview.is_attached());
        assert!(self.preview.failure().is_none());
    }

    /// Returns a stable slot identity for a structural change.
    fn first_slot(&self) -> SlotId {
        self.chain.slots().first().expect("first slot").id()
    }
}

#[test]
fn attached_clap_preview_retires_before_structural_replacement() {
    plugin_dispatcher()
        .call(
            |ui| {
                let mut fixture = Fixture::new(ui);
                fixture.replace(ui);
                fixture.assert_gain(0.25);
                let saved = fixture.chain.chain();
                let error = fixture
                    .chain
                    .preview_chain(ui)
                    .err()
                    .expect("existing insert owns CLAP lease");
                assert!(matches!(
                    error.downcast_ref::<audiobatch_plugins::PreviewChainError>(),
                    Some(audiobatch_plugins::PreviewChainError {
                        source: HostError::Clap(_),
                        ..
                    })
                ));
                assert!(fixture.chain.move_slot(fixture.first_slot(), 1));
                fixture.replace(ui);
                fixture.assert_gain(0.25);
                assert_eq!(
                    fixture.chain.chain(),
                    saved,
                    "identical slots retain their saved settings"
                );
                assert!(fixture.chain.set_enabled(fixture.first_slot(), false));
                fixture.replace(ui);
                fixture.assert_gain(0.5);
            },
            &|| false,
        )
        .expect("UI replacement regression");
}

#[test]
fn unchanged_clap_preview_can_be_replaced_repeatedly_without_losing_settings() {
    plugin_dispatcher()
        .call(
            |ui| {
                let mut fixture = Fixture::new(ui);
                fixture.replace(ui);
                let saved = fixture.chain.chain();
                // Compensation-only refreshes rebuild the same live chain, without a structural change.
                for _ in 0..3 {
                    fixture.replace(ui);
                    fixture.assert_gain(0.25);
                    assert_eq!(fixture.chain.chain(), saved);
                }
            },
            &|| false,
        )
        .expect("UI repeated replacement regression");
}
