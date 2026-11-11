/// AURealtime audits the exact build-tree AUv2 with startup-linked native RealtimeSanitizer.
/// Only native render, automation, reset, and detector boundaries are marked. Dependency reports remain unsuppressed.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fmt/format.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !__has_feature(realtime_sanitizer)
#error "ImageControlRSAURealtime must be compiled and linked with -fsanitize=realtime"
#endif
#if !__has_attribute(nonblocking)
#error "ImageControlRSAURealtime requires Clang's nonblocking attribute"
#endif

namespace esgrove::image_control::realtime
{
	namespace
	{
		constexpr UInt32 BLOCK_FRAMES{256};
		constexpr UInt32 LATENCY_FRAMES{4352};
		constexpr UInt32 SIGNAL_FRAMES{16'384};
		constexpr UInt32 TOTAL_FRAMES{SIGNAL_FRAMES + LATENCY_FRAMES};
		constexpr UInt32 PARAMETER_COUNT{31};
		constexpr double SAMPLE_RATE{48'000.0};
		constexpr unsigned int PROCESS_TIMEOUT_SECONDS{40};
		constexpr AudioComponentDescription DESCRIPTION{
		    .componentType = 0x61756678,
		    .componentSubType = 0x45676972,
		    .componentManufacturer = 0x45736772,
		    .componentFlags = 0,
		    .componentFlagsMask = 0,
		};

		/// Releases setup-only Core Foundation create/copy references outside every marked callback.
		struct CFDeleter
		{
			/// Balances a native owning reference without attempting module unloading.
			void operator()(const void* value) const
			{
				if (value != nullptr)
				{
					CFRelease(value);
				}
			}
		};
		using CFObject = std::unique_ptr<const void, CFDeleter>;

		/// Owns one serialized native instance until all callback threads have joined.
		class Unit
		{
		public:
			explicit Unit(AudioComponent component);
			~Unit();

			Unit(const Unit&) = delete;
			Unit& operator=(const Unit&) = delete;
			Unit(Unit&&) = delete;
			Unit& operator=(Unit&&) = delete;

			/// AudioToolbox owns this nonnull instance between construction and destruction.
			AudioUnit instance{nullptr};
		};

		/// Owns preallocated deterministic source planes pulled synchronously by the AU.
		struct Input
		{
			/// Source planes survive setup, cold callbacks, latency drains, and reset.
			std::array<std::vector<float>, 2> planes;
			/// Source offset is changed only between synchronous render calls.
			UInt32 offset{0};
			/// Matching noninterleaved input and output channel count.
			UInt32 channels{0};
		};

		/// Provides the native variable-length AudioBufferList ABI with capacity for two descriptors.
		struct StereoBuffers
		{
			/// Number of descriptors exposed to AudioUnitRender, one or two.
			UInt32 count;
			/// Descriptors refer to host-owned output storage for the duration of the render.
			std::array<AudioBuffer, 2> buffers;
		};
		static_assert(offsetof(StereoBuffers, buffers) == offsetof(AudioBufferList, mBuffers));

		/// Raises a setup or post-callback assertion, including in optimized builds.
		void require(bool condition, std::string_view message)
		{
			if (!condition)
			{
				throw std::runtime_error(std::string(message));
			}
		}

		/// Turns a native failure into a diagnostic only after leaving the marked region.
		void check(OSStatus status, std::string_view operation)
		{
			if (status != noErr)
			{
				throw std::runtime_error(fmt::format("{} failed with OSStatus {}", operation, status));
			}
		}

		/// Creates from the directly returned process-local component, not installed-component discovery.
		Unit::Unit(AudioComponent component)
		{
			check(AudioComponentInstanceNew(component, &instance), "AudioComponentInstanceNew");
		}

		/// Tears down native ownership on the main thread after all callback work has finished.
		Unit::~Unit()
		{
			if (instance != nullptr)
			{
				(void)AudioComponentInstanceDispose(instance);
			}
		}

		/// Reads setup-only fixed-size properties without placing allocation or diagnostics inside a callback.
		template <typename Value>
		Value property(
		    AudioUnit unit,
		    AudioUnitPropertyID propertyId,
		    AudioUnitScope scope = kAudioUnitScope_Global,
		    AudioUnitElement element = 0
		)
		{
			Value result{};
			auto bytes = static_cast<UInt32>(sizeof(result));
			check(AudioUnitGetProperty(unit, propertyId, scope, element, &result, &bytes), "AudioUnitGetProperty");
			require(bytes == sizeof(result), "Unexpected property size");
			return result;
		}

		/// Writes setup-only fixed-size properties while the unit is stopped.
		template <typename Value>
		void setProperty(AudioUnit unit, AudioUnitPropertyID propertyId, const Value& value, AudioUnitScope scope = kAudioUnitScope_Global)
		{
			check(AudioUnitSetProperty(unit, propertyId, scope, 0, &value, static_cast<UInt32>(sizeof(value))), "AudioUnitSetProperty");
		}

		/// Converts borrowed bundle metadata while its owning CFBundle remains loaded.
		std::string stringValue(CFStringRef value)
		{
			require(value != nullptr && CFGetTypeID(value) == CFStringGetTypeID(), "Missing bundle string metadata");
			std::array<char, 1024> storage{};
			require(
			    CFStringGetCString(value, storage.data(), static_cast<CFIndex>(storage.size()), kCFStringEncodingUTF8) != 0u,
			    "Invalid bundle UTF-8 metadata"
			);
			return storage.data();
		}

		/// Loads and registers only the explicit factory.
		/// No installation, registry service, or component discovery is used.
		/// A successful registration retains its CFBundle for process life, including exceptional instance teardown.
		AudioComponent loadBundle(const std::filesystem::path& path)
		{
			require(
			    path.is_absolute() && path.extension() == ".component" && std::filesystem::is_directory(path),
			    "--bundle requires an existing absolute .component directory"
			);
			const auto name = path.string();
			const CFObject url(CFURLCreateFromFileSystemRepresentation(
			    nullptr,
			    reinterpret_cast<const UInt8*>(name.data()),
			    static_cast<CFIndex>(name.size()),
			    1u
			));
			require(url != nullptr, "Could not create bundle URL");
			auto* const bundle = CFBundleCreate(nullptr, static_cast<CFURLRef>(url.get()));
			CFObject owner(bundle);
			require(bundle != nullptr && CFBundleLoadExecutable(bundle) != 0u, "Could not load exact component executable");
			require(stringValue(CFBundleGetIdentifier(bundle)) == "com.esgrove.imagecontrol-rs-own.au", "Unexpected AU bundle identity");
			const auto* info = CFBundleGetInfoDictionary(bundle);
			require(info != nullptr, "Missing bundle metadata");
			require(
			    stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("CFBundleShortVersionString")))) ==
			        IMAGE_CONTROL_VERSION,
			    "Unexpected AU bundle version"
			);
			const auto* components = static_cast<CFArrayRef>(CFDictionaryGetValue(info, CFSTR("AudioComponents")));
			require(
			    components != nullptr && CFGetTypeID(components) == CFArrayGetTypeID() && CFArrayGetCount(components) == 1,
			    "Expected exactly one native AU description"
			);
			const auto* description = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(components, 0));
			require(description != nullptr && CFGetTypeID(description) == CFDictionaryGetTypeID(), "Invalid AU description");
			require(
			    stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR("type")))) == "aufx" &&
			        stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR("subtype")))) == "Egir" &&
			        stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR("manufacturer")))) == "Esgr",
			    "Unexpected AU component identity"
			);
			const auto* factoryName = static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR("factoryFunction")));
			require(factoryName != nullptr && CFGetTypeID(factoryName) == CFStringGetTypeID(), "Missing AU factory name");
			auto factory = reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle, factoryName));
			require(
			    factory != nullptr && CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry")) != nullptr,
			    "The exact AU factory or static clap_entry export is missing"
			);
			auto* component = AudioComponentRegister(&DESCRIPTION, CFSTR("Esgrove: ImageControl RS RTSan isolated host"), 256, factory);
			require(component != nullptr, "Process-local AudioComponentRegister failed");
			[[maybe_unused]] const auto* processLifetimeBundle = owner.release();
			fmt::print("BUNDLE {} (aufx/Egir/Esgr, {})\n", name, IMAGE_CONTROL_VERSION);
			return component;
		}

		/// Rejects accidental multiple startup runtimes before loading a plugin with instrumented Rust and native code.
		void verifyRuntime()
		{
			unsigned int count = 0;
			for (std::uint32_t index = 0; index < _dyld_image_count(); ++index)
			{
				const std::string_view image(_dyld_get_image_name(index));
				if (image.contains("libclang_rt.rtsan_osx_dynamic.dylib"))
				{
					++count;
				}
			}
			require(count == 1, "Expected exactly one startup-linked native RTSan runtime");
		}

		/// Runs cold callbacks on a new thread, propagating failures after joining and outside every marked region.
		template <typename Work>
		void onFreshThread(Work work)
		{
			std::exception_ptr failure;
			std::thread thread([&] {
				try
				{
					work();
				}
				catch (...)
				{
					failure = std::current_exception();
				}
			});
			thread.join();
			if (failure)
			{
				std::rethrow_exception(failure);
			}
		}

		/// Marks only observable arithmetic, proving the loaded detector also accepts a clean callback.
		[[clang::noinline]] int clean_region(int value) noexcept [[clang::nonblocking]]
		{
			return value * 3 + 1;
		}

		/// Calls malloc through a volatile function pointer so optimization cannot remove the detector control.
		[[clang::noinline]] void* allocation_region() noexcept [[clang::nonblocking]]
		{
			void* (*volatile allocate)(std::size_t) = &std::malloc;
			return allocate(64);
		}

		/// Locks an already initialized native mutex, isolating the lock detector from lazy mutex allocation.
		[[clang::noinline]] int mutex_region(pthread_mutex_t& mutex) noexcept [[clang::nonblocking]]
		{
			const int status = pthread_mutex_lock(&mutex);
			return status == 0 ? pthread_mutex_unlock(&mutex) : status;
		}

		/// Executes one detector control without loading a bundle or warming the callback thread.
		void runControl(std::string_view scenario)
		{
			if (scenario == "clean-control")
			{
				int result = 0;
				onFreshThread([&] {
					result = clean_region(7);
				});
				require(result == 22, "Clean detector arithmetic failed");
			}
			else if (scenario == "allocation-control")
			{
				void* storage = nullptr;
				onFreshThread([&] {
					storage = allocation_region();
				});
				require(storage != nullptr, "Allocation control returned null");
				std::free(storage);
			}
			else
			{
				pthread_mutex_t mutex;
				require(pthread_mutex_init(&mutex, nullptr) == 0, "Could not initialize detector mutex");
				require(pthread_mutex_lock(&mutex) == 0 && pthread_mutex_unlock(&mutex) == 0, "Could not prepare detector mutex");
				int status = -1;
				onFreshThread([&] {
					status = mutex_region(mutex);
				});
				const int destroyStatus = pthread_mutex_destroy(&mutex);
				require(status == 0 && destroyStatus == 0, "Mutex detector control failed");
			}
		}

		/// Marks exactly the native automation call, including all upstream AU-to-CLAP event construction.
		[[clang::noinline]] OSStatus au_set_parameter_region(AudioUnit unit, AudioUnitParameterID parameterId, float value) noexcept
		    [[clang::nonblocking]]
		{
			return AudioUnitSetParameter(unit, parameterId, kAudioUnitScope_Global, 0, value, 0);
		}

		/// Marks exactly the native render call, including the wrapper, synchronous input pull, and Rust processing.
		[[clang::noinline]] OSStatus au_render_region(
		    AudioUnit unit,
		    AudioUnitRenderActionFlags& flags,
		    const AudioTimeStamp& timestamp,
		    UInt32 frames,
		    AudioBufferList& buffers
		) noexcept [[clang::nonblocking]]
		{
			return AudioUnitRender(unit, &flags, &timestamp, 0, frames, &buffers);
		}

		/// Marks exactly native reset, keeping history seeding and all result assertions outside this boundary.
		[[clang::noinline]] OSStatus au_reset_region(AudioUnit unit) noexcept [[clang::nonblocking]]
		{
			return AudioUnitReset(unit, kAudioUnitScope_Global, 0);
		}

		/// Supplies already allocated input planes without diagnostics, allocation, locks, or thread-local host state.
		OSStatus pullInput(
		    void* context,
		    AudioUnitRenderActionFlags* /*flags*/,
		    const AudioTimeStamp* /*timestamp*/,
		    UInt32 /*bus*/,
		    UInt32 frames,
		    AudioBufferList* buffers
		)
		{
			auto& input = *static_cast<Input*>(context);
			if (buffers->mNumberBuffers != input.channels || input.offset > input.planes[0].size() ||
			    frames > input.planes[0].size() - input.offset)
			{
				return kAudio_ParamError;
			}
			for (UInt32 channel = 0; channel < input.channels; ++channel)
			{
				buffers->mBuffers[channel] = {
				    .mNumberChannels = 1,
				    .mDataByteSize = frames * static_cast<UInt32>(sizeof(float)),
				    .mData = std::span(input.planes[channel]).subspan(input.offset).data(),
				};
			}
			return noErr;
		}

		/// Allocates seeded nonzero source data and configures one stopped matching mono/stereo unit at 48 kHz.
		void configure(AudioUnit unit, Input& input, UInt32 channels)
		{
			input.channels = channels;
			std::uint32_t random = 0x9e3779b9;
			for (auto& plane : input.planes)
			{
				plane.assign(TOTAL_FRAMES, 0.0F);
				for (UInt32 frame = 0; frame < SIGNAL_FRAMES; ++frame)
				{
					random ^= random << 13U;
					random ^= random >> 17U;
					random ^= random << 5U;
					plane[frame] = static_cast<float>(static_cast<std::int32_t>(random >> 8U) - 0x7fffff) * 0x1p-25F;
				}
			}
			AudioStreamBasicDescription format{};
			format.mSampleRate = SAMPLE_RATE;
			format.mFormatID = kAudioFormatLinearPCM;
			format.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) |
			                      static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
			format.mBytesPerPacket = sizeof(float);
			format.mFramesPerPacket = 1;
			format.mBytesPerFrame = sizeof(float);
			format.mChannelsPerFrame = channels;
			format.mBitsPerChannel = 32;
			setProperty(unit, kAudioUnitProperty_StreamFormat, format, kAudioUnitScope_Input);
			setProperty(unit, kAudioUnitProperty_StreamFormat, format, kAudioUnitScope_Output);
			setProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, BLOCK_FRAMES);
			const AURenderCallbackStruct callback{.inputProc = pullInput, .inputProcRefCon = &input};
			setProperty(unit, kAudioUnitProperty_SetRenderCallback, callback, kAudioUnitScope_Input);
			check(AudioUnitInitialize(unit), "AudioUnitInitialize");
			require(
			    std::abs(property<Float64>(unit, kAudioUnitProperty_Latency) * SAMPLE_RATE - static_cast<double>(LATENCY_FRAMES)) < 1.0e-6,
			    "AU must report 4352 latency frames at 48 kHz"
			);
			require(property<UInt32>(unit, kAudioUnitProperty_MaximumFramesPerSlice) == BLOCK_FRAMES, "Maximum slice size changed");
		}

		/// Returns independent neutral plain values while exercising every stable native sound-parameter ID.
		float neutralValue(AudioUnitParameterID parameterId)
		{
			constexpr std::array crossoverDefaults{100.0F, 500.0F, 5000.0F};
			if (parameterId >= 1 && parameterId <= 3)
			{
				return crossoverDefaults[parameterId - 1];
			}
			return parameterId >= 4 && parameterId < 28 && (parameterId - 4) % 6 == 2 ? 1.0F : 0.0F;
		}

		/// Prepares descriptors off the marked path, including every block's host-owned noninterleaved output storage.
		StereoBuffers outputBuffers(std::array<std::array<float, BLOCK_FRAMES>, 2>& output, UInt32 channels, UInt32 frames)
		{
			StereoBuffers buffers{.count = channels, .buffers = {}};
			for (UInt32 channel = 0; channel < channels; ++channel)
			{
				buffers.buffers[channel] = {
				    .mNumberChannels = 1,
				    .mDataByteSize = frames * static_cast<UInt32>(sizeof(float)),
				    .mData = output[channel].data(),
				};
			}
			return buffers;
		}

		/// Checks actual output storage and every delayed neutral bit after leaving the native callback boundary.
		void checkNeutral(const StereoBuffers& buffers, const Input& input, UInt32 offset, UInt32 frames)
		{
			require(buffers.count == input.channels, "Render changed output buffer count");
			for (UInt32 channel = 0; channel < input.channels; ++channel)
			{
				const auto& buffer = buffers.buffers[channel];
				require(
				    buffer.mData != nullptr && buffer.mDataByteSize >= frames * sizeof(float) && buffer.mNumberChannels == 1,
				    "Render returned invalid output storage"
				);
				const std::span samples(static_cast<const float*>(buffer.mData), frames);
				for (UInt32 frame = 0; frame < frames; ++frame)
				{
					const auto absolute = offset + frame;
					const auto expected = absolute < LATENCY_FRAMES ? 0.0F : input.planes[channel][absolute - LATENCY_FRAMES];
					require(
					    std::bit_cast<std::uint32_t>(samples[frame]) == std::bit_cast<std::uint32_t>(expected),
					    fmt::format("Neutral mismatch at channel {} sample {}", channel, absolute)
					);
				}
			}
		}

		/// Renders and drains real nonzero audio with no allocation inside the marked native call.
		/// Cold automation writes all 31 neutral targets before the first render on this fresh thread.
		void renderNeutral(AudioUnit unit, Input& input, bool automation)
		{
			std::array<std::array<float, BLOCK_FRAMES>, 2> output{};
			if (automation)
			{
				for (AudioUnitParameterID parameterId = 0; parameterId < PARAMETER_COUNT; ++parameterId)
				{
					check(au_set_parameter_region(unit, parameterId, neutralValue(parameterId)), "cold native automation");
				}
			}
			for (UInt32 offset = 0; offset < TOTAL_FRAMES; offset += BLOCK_FRAMES)
			{
				const auto frames = std::min(BLOCK_FRAMES, TOTAL_FRAMES - offset);
				auto buffers = outputBuffers(output, input.channels, frames);
				input.offset = offset;
				AudioTimeStamp timestamp{};
				timestamp.mSampleTime = static_cast<double>(offset);
				timestamp.mFlags = kAudioTimeStampSampleTimeValid;
				AudioUnitRenderActionFlags flags = 0;
				check(
				    au_render_region(unit, flags, timestamp, frames, *reinterpret_cast<AudioBufferList*>(&buffers)),
				    "marked native render"
				);
				checkNeutral(buffers, input, offset, frames);
			}
		}

		/// Delivers an audible gain change and verifies settled samples,
		/// rather than trusting wrapper-only parameter readback.
		void renderAutomation(AudioUnit unit, Input& input)
		{
			constexpr float gainDb = -6.0F;
			constexpr UInt32 settlingFrames = 2048;
			const float gain = std::pow(10.0F, gainDb * 0.05F);
			check(au_set_parameter_region(unit, 0, gainDb), "marked output automation");
			std::array<std::array<float, BLOCK_FRAMES>, 2> output{};
			UInt32 checked = 0;
			for (UInt32 offset = 0; offset < TOTAL_FRAMES; offset += BLOCK_FRAMES)
			{
				const auto frames = std::min(BLOCK_FRAMES, TOTAL_FRAMES - offset);
				auto buffers = outputBuffers(output, input.channels, frames);
				input.offset = offset;
				AudioTimeStamp timestamp{};
				timestamp.mSampleTime = static_cast<double>(TOTAL_FRAMES + offset);
				timestamp.mFlags = kAudioTimeStampSampleTimeValid;
				AudioUnitRenderActionFlags flags = 0;
				check(
				    au_render_region(unit, flags, timestamp, frames, *reinterpret_cast<AudioBufferList*>(&buffers)),
				    "automated native render"
				);
				require(buffers.count == input.channels, "Automated render changed output buffer count");
				for (UInt32 channel = 0; channel < input.channels; ++channel)
				{
					const auto& buffer = buffers.buffers[channel];
					require(
					    buffer.mData != nullptr && buffer.mDataByteSize >= frames * sizeof(float) && buffer.mNumberChannels == 1,
					    "Automated render returned invalid storage"
					);
					const std::span samples(static_cast<const float*>(buffer.mData), frames);
					for (UInt32 frame = 0; frame < frames; ++frame)
					{
						require(std::isfinite(samples[frame]), "Automation returned nonfinite audio");
						const auto absolute = offset + frame;
						if (absolute >= LATENCY_FRAMES + settlingFrames)
						{
							const auto expected = input.planes[channel][absolute - LATENCY_FRAMES] * gain;
							require(
							    std::abs(samples[frame] - expected) < 1.0e-6F,
							    "Output automation was not consumed by the Rust processor"
							);
							++checked;
						}
					}
				}
			}
			require(checked == input.channels * (SIGNAL_FRAMES - settlingFrames), "Incomplete settled automated audio check");
		}

		/// Seeds nonzero delay history on the setup thread solely to
		/// isolate reset's first operation on a fresh callback thread.
		/// This is not a cold-render probe.
		/// Cold render is independently marked in au-cold-automation without processing warmup.
		void seedResetHistory(AudioUnit unit, Input& input)
		{
			std::array<std::array<float, BLOCK_FRAMES>, 2> output{};
			auto buffers = outputBuffers(output, input.channels, BLOCK_FRAMES);
			input.offset = 0;
			AudioTimeStamp timestamp{};
			timestamp.mSampleTime = -static_cast<double>(BLOCK_FRAMES);
			timestamp.mFlags = kAudioTimeStampSampleTimeValid;
			AudioUnitRenderActionFlags flags = 0;
			check(
			    AudioUnitRender(unit, &flags, &timestamp, 0, BLOCK_FRAMES, reinterpret_cast<AudioBufferList*>(&buffers)),
			    "reset history setup"
			);
		}

		/// Audits mono and stereo in separate native instances,
		/// with setup and teardown serialized off callback threads.
		void runNative(AudioComponent component, bool reset)
		{
			for (UInt32 channels = 1; channels <= 2; ++channels)
			{
				const Unit unit(component);
				Input input;
				configure(unit.instance, input, channels);
				if (reset)
				{
					seedResetHistory(unit.instance, input);
				}
				onFreshThread([&] {
					if (reset)
					{
						check(au_reset_region(unit.instance), "marked native reset");
					}
					renderNeutral(unit.instance, input, !reset);
					if (!reset)
					{
						renderAutomation(unit.instance, input);
					}
				});
				check(AudioUnitUninitialize(unit.instance), "AudioUnitUninitialize");
				fmt::print(
				    "PASS {}-channel {} with nonzero input and complete latency drain\n",
				    channels,
				    reset ? "reset" : "cold automation"
				);
			}
		}

		/// Parses help before runtime or bundle work, then runs exactly one externally isolated case with a process deadline.
		int run(std::span<char*> arguments)
		{
			if (std::ranges::any_of(arguments.subspan(1), [](const char* argument) {
				    return std::string_view(argument) == "-h" || std::string_view(argument) == "--help";
			    }))
			{
				fmt::print(
				    "Usage: ImageControlRSAURealtime --case <name> [-b|--bundle <absolute.component>]\n"
				    "Cases: clean-control allocation-control mutex-control au-cold-automation au-reset\n"
				    "Native AU cases require the exact instrumented bundle. Detector controls do not load plugins.\n"
				    "Only process-local AudioComponentRegister is used. Dependency RTSan reports are not suppressed.\n"
				    "Checks detect exercised unsafe calls, not audio deadlines or spin-wait contention.\n"
				);
				return 0;
			}
			std::string_view scenario;
			std::filesystem::path bundle;
			for (std::size_t index = 1; index < arguments.size(); ++index)
			{
				const std::string_view argument(arguments[index]);
				if (argument == "--case" && index + 1 < arguments.size())
				{
					require(scenario.empty(), "--case may be supplied only once");
					scenario = arguments[++index];
				}
				else if ((argument == "-b" || argument == "--bundle") && index + 1 < arguments.size())
				{
					require(bundle.empty(), "--bundle may be supplied only once");
					bundle = arguments[++index];
				}
				else
				{
					throw std::runtime_error(fmt::format("Unknown or incomplete option: {}", argument));
				}
			}
			const bool control = scenario == "clean-control" || scenario == "allocation-control" || scenario == "mutex-control";
			require(control || scenario == "au-cold-automation" || scenario == "au-reset", "--case requires a supported case name");
			(void)alarm(PROCESS_TIMEOUT_SECONDS);
			verifyRuntime();
			if (control)
			{
				runControl(scenario);
			}
			else
			{
				runNative(loadBundle(bundle), scenario == "au-reset");
			}
			(void)alarm(0);
			fmt::print("CASE_OK {}\n", scenario);
			return 0;
		}
	} // namespace
} // namespace esgrove::image_control::realtime

/// Reports host failures outside marked regions. Actual sanitizer reports are emitted directly by the native runtime.
int main(int argc, char* argv[])
{
	try
	{
		return esgrove::image_control::realtime::run(std::span(argv, static_cast<std::size_t>(argc)));
	}
	catch (const std::exception& error)
	{
		fmt::print(stderr, "FAIL {}\n", error.what());
		return 1;
	}
}
