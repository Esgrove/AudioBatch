# AudioBatch Test MS

A minimal JUCE VST3 effect that the C++ and the Rust test suites both host.
Its processing is plain float arithmetic,
so `../fixtures/plugin_expected.py` can compute exactly what it must output without any plugin host.
Both suites null test their processed files against those expected files,
so passing both means the two applications process audio the same way.

The plugin is never installed into a system plugin folder.
It only shows up where a test points to it.

## Parameters

| Index | Name    | Range       | Default |
| ----- | ------- | ----------- | ------- |
| 0     | Gain    | 0 to 2      | 1       |
| 1     | Width   | 0 to 2      | 1       |
| 2     | Latency | 0 to 1024   | 0       |

Legacy parameter ids make the VST3 parameter ids the same as the indices, which the tests rely on.
Gain and width are linear, so a host sets them with the plain value divided by 2.
The latency is in samples, so a host sets it with the value divided by 1024.

## Processing

A stereo input is split into mid and side, the side is scaled by the width,
and both output channels are scaled by the gain.
A mono input is only scaled by the gain.
The default gain and width are neutral apart from floating-point rounding.

The output is then delayed by the latency, which the plugin reads when it is prepared and reports to the host.
A host that compensates the latency renders the delayed tail and drops the delayed start,
so it writes the same samples as the plugin without latency.
The latency case of the tests checks exactly that, and also that the plugin reports the latency.

Each operation is a float multiply or add in the order `plugin_expected.py` repeats them.
The plugin target explicitly disables contraction with `-ffp-contract=off` for non-MSVC builds only.
That setting does not establish an MSVC guarantee, so exact output still needs verification with each compiler.
The plugin's state is the XML of its parameter tree.

## Building

The C++ project builds the plugin as part of its tests,
and passes the bundle path to them as `AUDIOBATCH_TEST_VST3_PATH`.
The plugin requires CMake 4.2 and C++23, with a minimum deployment target of macOS 15 when built there.

The Rust tests need it built separately:

```shell
./build.sh
```

The script builds the Release bundle into `cmake-build-release/` with the JUCE submodule in `../cpp/JUCE`,
and prints its path, which `--print` prints without building.
Use `./build.sh --debug` or `./build.sh -d` for a Debug bundle in `cmake-build-debug/`,
leaving the default Release bundle untouched.
`./build.sh --debug --print` prints the Debug bundle path without building.
The Rust `test-fixtures` crate automatically finds only the default Release bundle.
To use Debug or another location, set `AUDIOBATCH_TEST_VST3` to the full bundle path,
for example `export AUDIOBATCH_TEST_VST3="$(./build.sh --debug --print)"` from this directory.
Without the bundle the Rust plugin tests skip, except in CI, where they fail.

Use the C++ project's [lint and format commands](../cpp/AGENTS.md#formatting) for this plugin.

### Optional VST3 Validation

In a standalone CMake build, configure with `-DAUDIOBATCH_PLUGINVAL=ON`,
then explicitly build `AudioBatchTestPlugin_pluginval` with the selected build directory and configuration.
The target is absent when this plugin is added as a subdirectory of the C++ application.
The shared helper in `../cpp/cmake/Pluginval.cmake` fetches and builds pinned pluginval in the repository's `.deps/`
and validates the built VST3 at strictness level 5.
It requires Git, a working build toolchain, and network access for missing dependencies.
It is opt-in, not part of the normal plugin build or host tests.

## Changing the Processing

When the processing changes, keep the expected outputs in step:

1. Update `../fixtures/plugin_expected.py` to repeat the new arithmetic.
2. Regenerate the expected files with `uv run fixtures/plugin_expected.py` from the repository root.
3. Update the expectation lists in `../cpp/tests/PluginPresetTests.cpp` and in the Rust `test-fixtures` crate.
4. Review the changed plugin-output entries in `../fixtures/checksums.sha256`,
   then use the scoped capture and verification procedure in [Test fixtures](../fixtures/README.md#regeneration-provenance).
   Review any affected metric expectations and regression goldens independently.

The format workflow in CI fails when `../fixtures/plugin_expected/` differs from what the script writes.
