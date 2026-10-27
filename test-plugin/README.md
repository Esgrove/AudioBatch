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
The default settings leave the audio unchanged.

The output is then delayed by the latency, which the plugin reads when it is prepared and reports to the host.
A host that compensates the latency renders the delayed tail and drops the delayed start,
so it writes the same samples as the plugin without latency.
The latency case of the tests checks exactly that, and also that the plugin reports the latency.

Each operation is a separate float multiply or add in the order `plugin_expected.py` repeats them,
and the build turns off fused multiply-adds, so the expected outputs match to the last bit.
The plugin's state is the XML of its parameter tree.

## Building

The C++ project builds the plugin as part of its tests,
and passes the bundle path to them as `AUDIOBATCH_TEST_VST3_PATH`.

The Rust tests need it built separately:

```shell
./build.sh
```

The script builds the Release bundle into `build/` with the JUCE submodule in `../cpp/JUCE`,
and prints its path, which `--print` prints without building.
The Rust `test-fixtures` crate finds the bundle there,
or wherever the `AUDIOBATCH_TEST_VST3` variable points.
Without the bundle the Rust plugin tests skip, except in CI, where they fail.

The plugin's source compiles with strict warnings as errors, and `../cpp/lint.sh` checks it with clang-tidy.
`../cpp/format.sh` formats it.

## Changing the Processing

When the processing changes, keep the expected outputs in step:

1. Update `../fixtures/plugin_expected.py` to repeat the new arithmetic.
2. Regenerate the expected files with `uv run fixtures/plugin_expected.py` from the repository root.
3. Update the expectation lists in `../cpp/tests/PluginPresetTests.cpp` and in the Rust `test-fixtures` crate.

The format workflow in CI fails when `../fixtures/plugin_expected/` differs from what the script writes.
