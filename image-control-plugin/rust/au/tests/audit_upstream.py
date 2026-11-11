# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Qualify pinned upstream AU lifecycle limitations without changing third-party code.

This is an executable source audit, not a realtime-safety or behavioral test.
Expected BLOCKED findings remain blockers even when every source assertion passes.
Run with uv run --script tests/audit_upstream.py --source <fetched-wrapper-root>.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re

WRAPPER_REVISION = "ddba261acdb421e9963959aa970a10876515aa6a"
BLOBS = {
    "src/wrapasauv2.cpp": "1c10808a1be1693d0536314e4f68af1f63cd7d98",
    "src/detail/shared/spinlock.h": "3e718091b85e9d7bfd918f4fa2b5bff948355f5d",
}


def read_pinned(root: pathlib.Path, relative: str, expected: str) -> str:
    """Verify Git blob identity so the audit cannot silently qualify another wrapper source."""
    data = (root / relative).read_bytes()
    header = f"blob {len(data)}\0".encode()
    actual = hashlib.sha1(header + data, usedforsecurity=False).hexdigest()
    if actual != expected:
        message = f"{relative}: expected blob {expected}, got {actual}"
        raise AssertionError(message)
    return data.decode()


def function(source: str, signature: str) -> str:
    """Extract one top-level wrapper method, keeping assertions scoped to the affected lifecycle call."""
    start = source.index(signature)
    opening = source.index("{", start)
    # Wrapper methods close with an unindented brace. Inner blocks are indented.
    closing = source.index("\n}", opening)
    return source[opening : closing + 2]


def require(pattern: str, source: str, reason: str) -> None:
    """Fail closed when a pinned source contract is absent, rather than downgrading qualification."""
    if re.search(pattern, source, re.DOTALL) is None:
        raise AssertionError(reason)


def audit(root: pathlib.Path) -> None:
    """Report reset forwarding and bounded, explicitly unresolved state/offline/contention gates."""
    sources = {relative: read_pinned(root, relative, blob) for relative, blob in BLOBS.items()}
    wrapper = sources["src/wrapasauv2.cpp"]
    reset = function(wrapper, "OSStatus WrapAsAUV2::Reset(")
    require(r"Base::Reset\(scope, element\)", reset, "AU reset must reset its base state")
    require(r"if \(_initialized\) _plugin->reset\(\)", reset, "Initialized AU reset must reach CLAP")
    require(r"_processAdapter->reset\(\)", reset, "Adapter event history must reset")
    require(r"SpinLockGuard processGuard\(_processLock\)", reset, "Reset lock audit changed")
    print("PASS pinned reset forwards to CLAP and resets adapter history")

    for name in ("SaveState", "RestoreState"):
        body = function(wrapper, f"OSStatus WrapAsAUV2::{name}(")
        require(
            r"if \(!IsInitialized\(\)\) return kAudioUnitErr_Uninitialized;",
            body,
            f"{name}: pre-initialization state audit changed",
        )
    restore = function(wrapper, "OSStatus WrapAsAUV2::RestoreState(")
    require(r"_ext\._state->load\(_plugin->_plugin, chunk\);", restore, "State load path changed")
    if re.search(r"(?:if|return)[^\n]*_state->load", restore):
        message = "RestoreState failure propagation changed. Requalify its limitation"
        raise AssertionError(message)
    print("BLOCKED state: save and restore reject pre-initialization access; restore ignores CLAP load's bool result")

    # The AU implementation never refers to the render extension or the AU offline property.
    if any(token in wrapper for token in ("kAudioUnitProperty_OfflineRender", "_ext._render", "CLAP_RENDER_OFFLINE")):
        message = "Offline mode forwarding changed. Requalify rather than assuming it works"
        raise AssertionError(message)
    print("BLOCKED offline: AU does not forward OfflineRender to CLAP render.set(OFFLINE)")

    render = function(wrapper, "OSStatus WrapAsAUV2::Render(")
    require(r"std::try_to_lock", render, "Render admission audit changed")
    require(r"std::memset\(buffers\.mBuffers\[j\]\.mData, 0", render, "Contended render silence path changed")
    parameter = function(wrapper, "OSStatus WrapAsAUV2::SetParameter(")
    require(r"SpinLockGuard processGuard\(_processLock\)", parameter, "Parameter lock audit changed")
    spin = sources["src/detail/shared/spinlock.h"]
    require(r"while \(locked_\.exchange\(true, std::memory_order_acquire\)\)", spin, "Spin wait audit changed")
    print(
        "BLOCKED contention: Render try-lock returns silence; SetParameter and Reset still use an unbounded spin lock"
    )
    print(
        f"PASS source identity and audit assertions for {WRAPPER_REVISION}. "
        "BLOCKED findings are not clean qualification"
    )


def main() -> None:
    """Parse the explicit fetched source path without fetching, installing, or editing any dependency."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-s", "--source", type=pathlib.Path, required=True, help="Pinned fetched clap-wrapper source root"
    )
    options = parser.parse_args()
    audit(options.source.resolve(strict=True))


if __name__ == "__main__":
    main()
