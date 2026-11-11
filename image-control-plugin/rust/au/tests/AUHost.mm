/// AUHost loads the exact build-tree AUv2 and qualifies its native ABI without installed-plugin discovery.
/// Every operation runs on this bounded console host's main thread. This is not a realtime-safety test.

#import <AppKit/AppKit.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace esgrove::image_control::test
{
	namespace
	{

		constexpr UInt32 PARAMETER_COUNT{31};
		constexpr UInt32 BLOCK_FRAMES{256};
		constexpr UInt32 LATENCY_FRAMES{4352};
		constexpr UInt32 SIGNAL_FRAMES{16'384};
		constexpr double SAMPLE_RATE{48'000.0};
		constexpr AudioComponentDescription DESCRIPTION{
		    .componentType = 0x61756678,
		    .componentSubType = 0x45676972,
		    .componentManufacturer = 0x45736772,
		    .componentFlags = 0,
		    .componentFlagsMask = 0,
		};
		constexpr std::array<std::string_view, PARAMETER_COUNT> PARAMETER_NAMES{
		    "Output Gain",    "Crossover 1", "Crossover 2", "Crossover 3",  "Band 1 Mid",     "Band 1 Side",    "Band 1 Width",
		    "Band 1 Recover", "Band 1 Pan",  "Band 1 Solo", "Band 2 Mid",   "Band 2 Side",    "Band 2 Width",   "Band 2 Recover",
		    "Band 2 Pan",     "Band 2 Solo", "Band 3 Mid",  "Band 3 Side",  "Band 3 Width",   "Band 3 Recover", "Band 3 Pan",
		    "Band 3 Solo",    "Band 4 Mid",  "Band 4 Side", "Band 4 Width", "Band 4 Recover", "Band 4 Pan",     "Band 4 Solo",
		    "Bypass",         "Delta",       "Mono",
		};

		/// Releases an owned Core Foundation object off the rendering path.
		struct CFDeleter
		{
			/// Balances one create/copy reference after native host work finishes.
			void operator()(const void* value) const
			{
				if (value != nullptr)
				{
					CFRelease(value);
				}
			}
		};
		using PropertyList = std::unique_ptr<const void, CFDeleter>;

		/// Owns one native AU instance, destroyed before its containing bundle reference.
		class Unit
		{
		public:
			explicit Unit(AudioComponent component);
			~Unit();
			Unit(const Unit&) = delete;
			Unit& operator=(const Unit&) = delete;
			Unit(Unit&&) = delete;
			Unit& operator=(Unit&&) = delete;

			/// AudioToolbox owns the pointed-to instance until this object's destructor.
			AudioUnit instance{nullptr};
		};

		/// Supplies deterministic preallocated input planes to AudioUnitRender's synchronous pull callback.
		struct Input
		{
			/// Entire source signal, which remains alive through every render call.
			std::array<std::vector<float>, 2> planes;
			/// Current first frame, set by the host before rendering.
			UInt32 offset{0};
			/// Negotiated matching input and output channel count.
			UInt32 channels{0};
		};

		/// Native variable-length stereo buffer list storage, owned by each synchronous host render.
		struct StereoBuffers
		{
			/// Number of valid channel descriptors passed to AudioToolbox.
			UInt32 count;
			/// Backing descriptors with the same ABI prefix as AudioBufferList.
			std::array<AudioBuffer, 2> buffers;
		};

		/// Throws a host-side assertion failure, never from a plugin callback.
		void require(bool condition, std::string_view message)
		{
			if (!condition)
			{
				throw std::runtime_error(std::string(message));
			}
		}

		/// Converts native failures into readable console diagnostics outside the rendering callback.
		void check(OSStatus status, std::string_view operation)
		{
			if (status != noErr)
			{
				throw std::runtime_error(fmt::format("{} failed with OSStatus {}", operation, status));
			}
		}

		/// Creates one instance from the exact bundle's process-local factory, without registry discovery.
		Unit::Unit(AudioComponent component)
		{
			check(AudioComponentInstanceNew(component, &instance), "AudioComponentInstanceNew");
		}

		/// Disposes the instance on the main thread after all synchronous render operations finish.
		Unit::~Unit()
		{
			if (instance != nullptr)
			{
				(void)AudioComponentInstanceDispose(instance);
			}
		}

		/// Reads a fixed-size native property on the serialized host thread.
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
			require(bytes == sizeof(result), "Property returned an unexpected size");
			return result;
		}

		/// Writes a fixed-size native property before processing or between serialized render calls.
		template <typename Value>
		void setProperty(
		    AudioUnit unit,
		    AudioUnitPropertyID propertyId,
		    const Value& value,
		    AudioUnitScope scope = kAudioUnitScope_Global,
		    AudioUnitElement element = 0
		)
		{
			check(
			    AudioUnitSetProperty(unit, propertyId, scope, element, static_cast<const void*>(&value), static_cast<UInt32>(sizeof(value))),
			    "AudioUnitSetProperty"
			);
		}

		/// Converts a borrowed CF name to UTF-8 without retaining it past its owner's lifetime.
		std::string stringValue(CFStringRef value)
		{
			require(value != nullptr, "Missing CFString metadata");
			std::array<char, 1024> buffer{};
			require(
			    CFStringGetCString(value, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8) != 0u,
			    "Invalid UTF-8 metadata"
			);
			return buffer.data();
		}

		/// Loads an absolute explicit bundle and resolves its factory, never selecting an installed component.
		/// AudioComponentRegister affects only this process. No installed registry or registration daemon is modified.
		AudioComponent loadBundle(const std::filesystem::path& path, PropertyList& bundleOwner)
		{
			require(path.is_absolute() && std::filesystem::is_directory(path), "--bundle requires an absolute component path");
			const auto pathString = path.string();
			const PropertyList url(CFURLCreateFromFileSystemRepresentation(
			    nullptr,
			    reinterpret_cast<const UInt8*>(pathString.data()),
			    static_cast<CFIndex>(pathString.size()),
			    1u
			));
			require(url != nullptr, "Could not create the bundle URL");
			auto* bundle = CFBundleCreate(nullptr, static_cast<CFURLRef>(url.get()));
			require(bundle != nullptr, "Could not open the exact component bundle");
			bundleOwner.reset(bundle);
			require(CFBundleLoadExecutable(bundle) != 0u, "Could not load the component executable");
			const auto* const identifier = CFBundleGetIdentifier(bundle);
			require(stringValue(identifier) == "com.esgrove.imagecontrol-rs-own.au", "Unexpected bundle identifier");
			const auto* info = CFBundleGetInfoDictionary(bundle);
			require(
			    stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("CFBundleShortVersionString")))) ==
			        IMAGE_CONTROL_VERSION,
			    "Unexpected bundle version"
			);
			const auto* components = static_cast<CFArrayRef>(CFDictionaryGetValue(info, CFSTR("AudioComponents")));
			require(components != nullptr && CFArrayGetCount(components) == 1, "Expected exactly one AU component");
			const auto* componentInfo = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(components, 0));
			require(
			    stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(componentInfo, CFSTR("type")))) == "aufx" &&
			        stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(componentInfo, CFSTR("subtype")))) == "Egir" &&
			        stringValue(static_cast<CFStringRef>(CFDictionaryGetValue(componentInfo, CFSTR("manufacturer")))) == "Esgr",
			    "Unexpected AudioComponents identity"
			);
			const auto* factoryName = static_cast<CFStringRef>(CFDictionaryGetValue(componentInfo, CFSTR("factoryFunction")));
			auto factory = reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle, factoryName));
			require(factory != nullptr, "The exact bundle's factory export is missing");
			require(CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry")) != nullptr, "Static clap_entry export is missing");
			// Use the returned local component directly.
			// AudioComponentFindNext could select a different installed artifact.
			auto* component = AudioComponentRegister(&DESCRIPTION, CFSTR("Esgrove: ImageControl RS isolated host"), 256, factory);
			require(component != nullptr, "Process-local factory registration failed");
			fmt::print("PASS exact build-tree bundle {} (aufx/Egir/Esgr, {})\n", pathString, IMAGE_CONTROL_VERSION);
			return component;
		}

		/// Verifies all stable IDs, names, flags, finite ranges and defaults independently of enumeration order.
		std::array<AudioUnitParameterInfo, PARAMETER_COUNT> metadata(AudioUnit unit)
		{
			UInt32 bytes = 0;
			Boolean writable = 0u;
			check(
			    AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &bytes, &writable),
			    "ParameterList size"
			);
			require(bytes == PARAMETER_COUNT * sizeof(AudioUnitParameterID), "Expected exactly 31 sound parameters");
			std::array<AudioUnitParameterID, PARAMETER_COUNT> ids{};
			check(
			    AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &bytes),
			    "ParameterList"
			);
			std::ranges::sort(ids);
			std::array<AudioUnitParameterInfo, PARAMETER_COUNT> result{};
			for (UInt32 id = 0; id < PARAMETER_COUNT; ++id)
			{
				require(ids[id] == id, "Stable parameter IDs must be exactly 0 through 30");
				auto info = property<AudioUnitParameterInfo>(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id);
				const auto name = info.cfNameString != nullptr ? stringValue(info.cfNameString) : std::string(info.name);
				if ((info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0 && info.cfNameString != nullptr)
				{
					CFRelease(info.cfNameString);
				}
				info.cfNameString = nullptr;
				require(name == PARAMETER_NAMES[id], fmt::format("Parameter {} name '{}' differs from '{}'", id, name, PARAMETER_NAMES[id]));
				require(
				    std::isfinite(info.minValue) && std::isfinite(info.maxValue) && std::isfinite(info.defaultValue) &&
				        info.minValue < info.maxValue && info.defaultValue >= info.minValue && info.defaultValue <= info.maxValue,
				    "Invalid parameter range or default"
				);
				float expectedMinimum = 0.0F;
				float expectedMaximum = 1.0F;
				float expectedDefault = 0.0F;
				if (id == 0)
				{
					expectedMinimum = -24.0F;
					expectedMaximum = 12.0F;
				}
				else if (id <= 3)
				{
					expectedMinimum = 20.0F;
					expectedMaximum = 20'000.0F;
					constexpr std::array crossoverDefaults{100.0F, 500.0F, 5000.0F};
					expectedDefault = crossoverDefaults[id - 1];
				}
				else if (id < 28)
				{
					switch ((id - 4) % 6)
					{
						case 0:
						case 1:
							expectedMinimum = -60.0F;
							expectedMaximum = 12.0F;
							break;
						case 2:
							expectedMaximum = 2.0F;
							expectedDefault = 1.0F;
							break;
						case 3:
							expectedMaximum = 200.0F;
							break;
						case 4:
							expectedMinimum = -100.0F;
							expectedMaximum = 100.0F;
							break;
						case 5:
							break;
						default:
							throw std::runtime_error("Invalid band parameter field");
					}
				}
				require(
				    std::abs(info.minValue - expectedMinimum) < 1.0e-5F && std::abs(info.maxValue - expectedMaximum) < 1.0e-5F &&
				        std::abs(info.defaultValue - expectedDefault) < 1.0e-5F,
				    fmt::format("Parameter {} does not match its independent plain-unit range/default", id)
				);
				const auto requiredFlags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
				require((info.flags & requiredFlags) == requiredFlags, "A sound parameter is not readable and writable");
				result[id] = info;
			}
			fmt::print("PASS metadata IDs 0..30 and stable names\n");
			return result;
		}

		/// Fills callback buffers from preallocated source planes, without allocation, locking, or diagnostics.
		OSStatus pullInput(
		    void* context,
		    AudioUnitRenderActionFlags* /*flags*/,
		    [[maybe_unused]] const AudioTimeStamp* timestamp,
		    [[maybe_unused]] UInt32 bus,
		    UInt32 frames,
		    AudioBufferList* buffers
		)
		{
			auto& input = *static_cast<Input*>(context);
			if (buffers->mNumberBuffers != input.channels || input.offset + frames > input.planes[0].size())
			{
				return kAudio_ParamError;
			}
			for (UInt32 channel = 0; channel < input.channels; ++channel)
			{
				auto& buffer = buffers->mBuffers[channel];
				buffer.mNumberChannels = 1;
				buffer.mDataByteSize = frames * static_cast<UInt32>(sizeof(float));
				buffer.mData = std::span(input.planes[channel]).subspan(input.offset).data();
			}
			return noErr;
		}

		/// Configures one matching mono/stereo noninterleaved float layout while the unit is stopped.
		void configure(AudioUnit unit, UInt32 channels, Input& input)
		{
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
			input.channels = channels;
			const AURenderCallbackStruct callback{.inputProc = pullInput, .inputProcRefCon = &input};
			setProperty(unit, kAudioUnitProperty_SetRenderCallback, callback, kAudioUnitScope_Input);
		}

		/// Saves a retained state dictionary off the audio thread, preserving ownership through restore.
		PropertyList saveState(AudioUnit unit)
		{
			CFPropertyListRef state = nullptr;
			auto bytes = static_cast<UInt32>(sizeof(state));
			check(
			    AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, static_cast<void*>(&state), &bytes),
			    "state save"
			);
			require(state != nullptr && CFGetTypeID(state) == CFDictionaryGetTypeID(), "State is not a dictionary");
			return PropertyList(state);
		}

		/// Diagnoses the pinned wrapper's rejected pre-initialization save and restore, rather than hiding them.
		void checkUninitializedState(AudioUnit unit, CFPropertyListRef validState)
		{
			CFPropertyListRef state = nullptr;
			auto bytes = static_cast<UInt32>(sizeof(state));
			const auto saveStatus =
			    AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, static_cast<void*>(&state), &bytes);
			const PropertyList owner(state);
			const auto restoreStatus = AudioUnitSetProperty(
			    unit,
			    kAudioUnitProperty_ClassInfo,
			    kAudioUnitScope_Global,
			    0,
			    static_cast<const void*>(&validState),
			    static_cast<UInt32>(sizeof(validState))
			);
			require(
			    saveStatus == kAudioUnitErr_Uninitialized && restoreStatus == kAudioUnitErr_Uninitialized,
			    "Pinned pre-initialization state limitation changed. Requalify the upstream audit"
			);
			fmt::print("BLOCKED pre-initialization state save={} restore={} (upstream rejects both)\n", saveStatus, restoreStatus);
		}

		/// Sets one plain-unit native parameter outside rendering. The wrapper delivers it on the next render/flush.
		void setParameter(AudioUnit unit, AudioUnitParameterID parameterId, AudioUnitParameterValue value)
		{
			check(AudioUnitSetParameter(unit, parameterId, kAudioUnitScope_Global, 0, value, 0), "parameter write");
		}

		/// Reads one native plain-unit parameter, checking a state recall did not merely return success.
		float parameter(AudioUnit unit, AudioUnitParameterID parameterId)
		{
			AudioUnitParameterValue value = 0;
			check(AudioUnitGetParameter(unit, parameterId, kAudioUnitScope_Global, 0, &value), "parameter read");
			return value;
		}

		/// Renders the seeded input and compares every delayed output bit, including the latency drain.
		/// Host buffers and source histories are allocated before the first callback.
		void renderNeutral(AudioUnit unit, Input& input)
		{
			std::array<std::array<float, BLOCK_FRAMES>, 2> output{};
			StereoBuffers buffers{.count = input.channels, .buffers = {}};
			constexpr auto totalFrames = SIGNAL_FRAMES + LATENCY_FRAMES;
			UInt32 random = 0x9e3779b9;
			for (auto& plane : input.planes)
			{
				plane.assign(totalFrames + BLOCK_FRAMES, 0.0F);
				for (UInt32 frame = 0; frame < SIGNAL_FRAMES; ++frame)
				{
					random ^= random << 13U;
					random ^= random >> 17U;
					random ^= random << 5U;
					plane[frame] = static_cast<float>(static_cast<std::int32_t>(random >> 8U) - 0x7fffff) * 0x1p-25F;
				}
			}
			for (UInt32 offset = 0; offset < totalFrames; offset += BLOCK_FRAMES)
			{
				const auto frames = std::min(BLOCK_FRAMES, totalFrames - offset);
				input.offset = offset;
				for (UInt32 channel = 0; channel < input.channels; ++channel)
				{
					buffers.buffers[channel] = {
					    .mNumberChannels = 1,
					    .mDataByteSize = frames * static_cast<UInt32>(sizeof(float)),
					    .mData = output[channel].data(),
					};
				}
				AudioTimeStamp timestamp{};
				timestamp.mSampleTime = static_cast<double>(offset);
				timestamp.mFlags = kAudioTimeStampSampleTimeValid;
				AudioUnitRenderActionFlags flags = 0;
				check(AudioUnitRender(unit, &flags, &timestamp, 0, frames, reinterpret_cast<AudioBufferList*>(&buffers)), "render");
				for (UInt32 channel = 0; channel < input.channels; ++channel)
				{
					for (UInt32 frame = 0; frame < frames; ++frame)
					{
						const auto absolute = offset + frame;
						const auto expected = absolute < LATENCY_FRAMES ? 0.0F : input.planes[channel][absolute - LATENCY_FRAMES];
						require(
						    std::bit_cast<std::uint32_t>(output[channel][frame]) == std::bit_cast<std::uint32_t>(expected),
						    fmt::format("Neutral mismatch at channel {} sample {}", channel, absolute)
						);
					}
				}
			}
			fmt::print("PASS {}-channel neutral output, bit-exact latency and drain\n", input.channels);
		}

		/// Seeds a fresh nonzero delayed history immediately before reset, so a no-op reset cannot pass.
		void dirtyHistory(AudioUnit unit, Input& input)
		{
			std::array<std::array<float, BLOCK_FRAMES>, 2> output{};
			StereoBuffers buffers{.count = input.channels, .buffers = {}};
			for (UInt32 channel = 0; channel < input.channels; ++channel)
			{
				buffers.buffers[channel] = {
				    .mNumberChannels = 1,
				    .mDataByteSize = BLOCK_FRAMES * static_cast<UInt32>(sizeof(float)),
				    .mData = output[channel].data(),
				};
			}
			input.offset = 0;
			AudioTimeStamp timestamp{};
			timestamp.mSampleTime = 30'000.0;
			timestamp.mFlags = kAudioTimeStampSampleTimeValid;
			AudioUnitRenderActionFlags flags = 0;
			check(
			    AudioUnitRender(unit, &flags, &timestamp, 0, BLOCK_FRAMES, reinterpret_cast<AudioBufferList*>(&buffers)),
			    "seed history before reset"
			);
		}

		/// Qualifies an initialized state round trip with an audible parameter changed in both directions.
		PropertyList checkInitializedState(
		    AudioUnit unit,
		    Input& input,
		    const std::array<AudioUnitParameterInfo, PARAMETER_COUNT>& parameters
		)
		{
			auto neutral = saveState(unit);
			setParameter(unit, 0, parameters[0].maxValue);
			// A zero-input render delivers queued automation before the wrapper invokes CLAP state.save().
			std::array<float, BLOCK_FRAMES> output{};
			AudioBufferList buffers{
			    .mNumberBuffers = 1,
			    .mBuffers = {{.mNumberChannels = 1, .mDataByteSize = static_cast<UInt32>(sizeof(output)), .mData = output.data()}},
			};
			if (input.channels == 1)
			{
				AudioTimeStamp timestamp{};
				timestamp.mFlags = kAudioTimeStampSampleTimeValid;
				timestamp.mSampleTime = SIGNAL_FRAMES + LATENCY_FRAMES + BLOCK_FRAMES;
				input.offset = SIGNAL_FRAMES + LATENCY_FRAMES;
				AudioUnitRenderActionFlags flags = 0;
				check(AudioUnitRender(unit, &flags, &timestamp, 0, BLOCK_FRAMES, &buffers), "state automation delivery");
			}
			else
			{
				// The round trip is performed once in mono. Stereo receives the same saved neutral state.
				throw std::runtime_error("State qualification requires its mono test instance");
			}
			const auto changed = saveState(unit);
			const auto* state = static_cast<CFPropertyListRef>(neutral.get());
			setProperty(unit, kAudioUnitProperty_ClassInfo, state);
			require(std::abs(parameter(unit, 0) - parameters[0].defaultValue) < 1.0e-5F, "Neutral state restore was not applied");
			state = static_cast<CFPropertyListRef>(changed.get());
			setProperty(unit, kAudioUnitProperty_ClassInfo, state);
			require(std::abs(parameter(unit, 0) - parameters[0].maxValue) < 1.0e-5F, "Changed state restore was not applied");
			state = static_cast<CFPropertyListRef>(neutral.get());
			setProperty(unit, kAudioUnitProperty_ClassInfo, state);
			fmt::print("PASS initialized state round trip with changed Output\n");
			return neutral;
		}

		/// Opens the optional Cocoa editor, pumps native events briefly, and closes it twice on the main thread.
		void checkEditor(AudioUnit unit)
		{
			UInt32 bytes = 0;
			Boolean writable = 0u;
			check(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &bytes, &writable), "CocoaUI size");
			require(bytes >= sizeof(AudioUnitCocoaViewInfo), "CocoaUI descriptor is missing");
			std::vector<std::byte> storage(bytes);
			auto* info = reinterpret_cast<AudioUnitCocoaViewInfo*>(storage.data());
			check(AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &bytes), "CocoaUI");
			const PropertyList bundleURL(info->mCocoaAUViewBundleLocation);
			const auto classCount = (bytes - offsetof(AudioUnitCocoaViewInfo, mCocoaAUViewClass)) / sizeof(CFStringRef);
			require(classCount > 0, "CocoaUI has no view class");
			const auto className = stringValue(info->mCocoaAUViewClass[0]);
			for (std::size_t index = 0; index < classCount; ++index)
			{
				CFRelease(info->mCocoaAUViewClass[index]);
			}
			[NSApplication sharedApplication];
			NSString* const nativeClassName = [NSString stringWithUTF8String:className.c_str()];
			if (nativeClassName == nil)
			{
				throw std::runtime_error("Invalid native editor class name");
			}
			const Class viewClass = NSClassFromString(nativeClassName);
			require(viewClass != Nil && [viewClass conformsToProtocol:@protocol(AUCocoaUIBase)], "Invalid Cocoa view factory");
			for (int iteration = 0; iteration < 2; ++iteration)
			{
				const id<AUCocoaUIBase> factory = [[viewClass alloc] init];
				NSView* const view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(1000, 764)];
				require(view != nil, "Editor creation failed");
				NSWindow* const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1000, 764)
				                                                     styleMask:NSWindowStyleMaskTitled
				                                                       backing:NSBackingStoreBuffered
				                                                         defer:NO];
				[window setReleasedWhenClosed:NO];
				[window setContentView:view];
				[window orderFront:nil];
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
				require([[view subviews] count] == 1, "AU editor did not attach exactly one native child");
				require(![[[view subviews] objectAtIndex:0] isHidden], "AU editor child stayed hidden without CLAP gui.show");
				[view setFrameSize:NSMakeSize(1100, 800)];
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
				require([[view subviews] count] == 1, "AU editor resize replaced or lost its native child");
				[window setContentView:nil];
				[window close];
			}
			fmt::print("PASS optional native editor open/close twice\n");
		}

		/// Runs independent layout instances and preserves known upstream lifecycle limitations as explicit blockers.
		void qualify(AudioComponent component, bool editor)
		{
			PropertyList saved;
			for (UInt32 channels = 1; channels <= 2; ++channels)
			{
				const Unit unit(component);
				const auto parameters = metadata(unit.instance);
				Input input;
				configure(unit.instance, channels, input);
				if (saved != nullptr)
				{
					checkUninitializedState(unit.instance, static_cast<CFPropertyListRef>(saved.get()));
				}
				for (UInt32 id = 0; id < PARAMETER_COUNT; ++id)
				{
					setParameter(unit.instance, id, parameters[id].defaultValue);
				}
				check(AudioUnitInitialize(unit.instance), "initialize");
				const auto latency = property<Float64>(unit.instance, kAudioUnitProperty_Latency);
				require(
				    std::abs(latency * SAMPLE_RATE - static_cast<double>(LATENCY_FRAMES)) < 1.0e-6,
				    "Expected 4352-sample latency at 48 kHz"
				);
				renderNeutral(unit.instance, input);
				if (channels == 1)
				{
					saved = checkInitializedState(unit.instance, input, parameters);
					if (editor)
					{
						checkEditor(unit.instance);
					}
				}
				dirtyHistory(unit.instance, input);
				check(AudioUnitReset(unit.instance, kAudioUnitScope_Global, 0), "reset after nonzero history");
				renderNeutral(unit.instance, input);
				UInt32 offline = 1;
				const auto offlineStatus = AudioUnitSetProperty(
				    unit.instance,
				    kAudioUnitProperty_OfflineRender,
				    kAudioUnitScope_Global,
				    0,
				    &offline,
				    sizeof(offline)
				);
				fmt::print(
				    "BLOCKED offline render property status={} (no AU-to-CLAP render-mode forwarding in pinned source)\n",
				    offlineStatus
				);
				check(AudioUnitUninitialize(unit.instance), "uninitialize");
				checkUninitializedState(unit.instance, static_cast<CFPropertyListRef>(saved.get()));
			}
			fmt::print(
			    "BLOCKED contention: Render try-lock silences a missed block. SetParameter/Reset still spin. "
			    "See the pinned source audit. No realtime or deterministic-offline qualification is claimed.\n"
			);
		}

		/// Parses a narrow host command line, keeping editor qualification opt-in for desktop sessions.
		int run(std::span<char*> arguments)
		{
			std::filesystem::path bundle;
			bool editor = false;
			for (std::size_t index = 1; index < arguments.size(); ++index)
			{
				const std::string_view argument(arguments[index]);
				if (argument == "-h" || argument == "--help")
				{
					fmt::print(
					    "Usage: ImageControlRSAUHost -b|--bundle <absolute.component> [-e|--editor]\n"
					    "Loads only this bundle. Registers its factory in this process only, never in the installed registry.\n"
					    "Known upstream blockers are reported explicitly, not counted as clean qualification.\n"
					);
					return 0;
				}
				if ((argument == "-b" || argument == "--bundle") && index + 1 < arguments.size())
				{
					bundle = arguments[++index];
				}
				else if (argument == "-e" || argument == "--editor")
				{
					editor = true;
				}
				else
				{
					throw std::runtime_error(fmt::format("Unknown or incomplete option: {}", argument));
				}
			}
			PropertyList bundleOwner;
			auto* const component = loadBundle(bundle, bundleOwner);
			qualify(component, editor);
			fmt::print("PASS bounded AU functional contracts. BLOCKED upstream state/offline/contention gates remain.\n");
			// CFBundle is retained by process-local registration until process exit.
			// Arbitrary module unload is not tested.
			[[maybe_unused]] const auto* processLifetimeBundle = bundleOwner.release();
			return 0;
		}

	} // namespace
} // namespace esgrove::image_control::test

/// Runs the native qualification inside a main-thread autorelease pool, with bounded CTest process lifetime.
int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		try
		{
			return esgrove::image_control::test::run(std::span(argv, static_cast<std::size_t>(argc)));
		}
		catch (const std::exception& error)
		{
			fmt::print(stderr, "FAIL {}\n", error.what());
			return 1;
		}
	}
}
