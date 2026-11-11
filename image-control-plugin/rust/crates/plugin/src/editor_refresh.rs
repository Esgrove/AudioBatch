//! Change-filtered ordinary control polling, independent of native-rate spectrum and meter drawing.

use std::hash::Hash;
use std::sync::Arc;
use std::time::{Duration, Instant};

use iced::advanced::subscription::{self, EventStream, Hasher, Recipe};
use iced::futures::{StreamExt as _, stream};
use iced_baseview as iced;
use image_control_dsp::SharedState;

use super::Message;
use crate::params::{PARAMETER_COUNT, Parameters};

/// Ordinary external controls need not rebuild more often than this, regardless of display frequency.
const CONTROL_INTERVAL: Duration = Duration::from_nanos(16_666_667);
/// Diagnostic text changes at a human-readable rate rather than on every visual frame.
const STATISTICS_INTERVAL: Duration = Duration::from_millis(500);

/// Retains only Send-safe observation sources. Host callbacks and GUI state never enter the executor.
struct ControlRefresh {
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    pending_work: bool,
    show_fps: bool,
    notifier: Option<iced::PollSubNotifier>,
}

/// Exact source values identify changes without serializing state or acquiring an audio-owned lock.
#[derive(PartialEq)]
struct Observation {
    parameters: [Option<u64>; PARAMETER_COUNT],
    restore_generation: u64,
    display: image_control_dsp::DisplayPreferences,
    layout: crate::editor_preferences::LayoutPreferences,
    size: crate::editor_geometry::LogicalSize,
    analysis_status: image_control_dsp::AnalysisStatus,
}

impl Observation {
    /// Reads independent atomics and detached immutable settings without a GUI borrow.
    fn read(parameters: &Parameters, shared: &SharedState) -> Self {
        Self {
            parameters: std::array::from_fn(|id| parameters.get(id as u32).map(f64::to_bits)),
            restore_generation: parameters.restore_generation(),
            display: shared.display_preferences(),
            layout: parameters.layout.preferences(),
            size: parameters.layout.snapshot(),
            analysis_status: shared.analysis_status(),
        }
    }
}

impl Recipe for ControlRefresh {
    type Output = Message;

    /// Instance identity and work mode keep independent editors' subscription histories separate.
    fn hash(&self, state: &mut Hasher) {
        std::any::TypeId::of::<Self>().hash(state);
        Arc::as_ptr(&self.parameters).hash(state);
        self.pending_work.hash(state);
        self.show_fps.hash(state);
    }

    /// Samples at most 60 Hz but emits only changes, active worker polls, or diagnostic-window updates.
    fn stream(self: Box<Self>, input: EventStream) -> iced::futures::stream::BoxStream<'static, Message> {
        let Some(recipe) = subscription::into_recipes(iced::poll_events()).into_iter().next() else {
            return stream::empty().boxed();
        };
        let polls = recipe.stream(input);
        let mut previous = Observation::read(&self.parameters, &self.shared);
        let mut checked = Instant::now();
        let mut statistics = checked;
        let mut initial = true;
        polls
            .filter_map(move |()| {
                if let Some(notifier) = &self.notifier {
                    notifier.notify();
                }
                let now = Instant::now();
                let mut changed = std::mem::take(&mut initial);
                if now.saturating_duration_since(checked) >= CONTROL_INTERVAL {
                    checked = now;
                    let observation = Observation::read(&self.parameters, &self.shared);
                    changed |= observation != previous;
                    previous = observation;
                    changed |= self.pending_work;
                }
                if self.show_fps && now.saturating_duration_since(statistics) >= STATISTICS_INTERVAL {
                    statistics = now;
                    changed = true;
                }
                std::future::ready(changed.then_some(Message::Refresh))
            })
            .boxed()
    }
}

/// Keeps no-op polling out of application updates while animations continue in retained widgets.
pub(super) fn subscription(
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    pending_work: bool,
    notifier: Option<iced::PollSubNotifier>,
) -> iced::Subscription<Message> {
    let show_fps = shared.display_preferences().show_fps;
    subscription::from_recipe(ControlRefresh {
        parameters,
        shared,
        pending_work,
        show_fps,
        notifier,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ordinary_observation_ignores_meter_publications_but_detects_external_controls() {
        let parameters = Parameters::default();
        let shared = image_control_dsp::Imager::new().shared();
        let before = Observation::read(&parameters, &shared);
        assert!(before == Observation::read(&parameters, &shared));
        parameters.set(crate::params::GAIN_PARAMETER, 0.4);
        assert!(before != Observation::read(&parameters, &shared));
    }

    #[test]
    fn unchanged_polls_emit_only_the_initial_synchronization() {
        use iced::futures::executor::block_on;
        let parameters = Arc::new(Parameters::default());
        let shared = image_control_dsp::Imager::new().shared();
        let notifier = iced::PollSubNotifier::new();
        let recipe = Box::new(ControlRefresh {
            parameters: Arc::clone(&parameters),
            shared,
            pending_work: false,
            show_fps: false,
            notifier: Some(notifier),
        });
        let input = stream::iter((0..10).map(|_| subscription::Event::Interaction {
            window: iced::window::Id::unique(),
            event: iced::Event::Window(iced::window::Event::Moved(iced::Point::ORIGIN)),
            status: iced::event::Status::Ignored,
        }))
        .boxed();
        let messages = block_on(recipe.stream(input).collect::<Vec<_>>());
        assert_eq!(messages.len(), 1);
        assert!(matches!(messages.first(), Some(Message::Refresh)));
    }
}
