# Pd loader symbol lifetimes

Delegated by win while mac is offline. The same-process stress test creates 200 distinct abstractions, loading adi.param with each, and closes each engine before the next. The unmodified pinned Pd crashes on Windows in this test (exit 0xc0000005).

Source inspection: s_loader.c keeps instance-owned symbols in a process-wide loaded list and compares pointer identity. pdinstance_free releases those symbols. A later symbol at the reused address produces a false cache hit; new_anything then recursively dispatches an unregistered creator. m_class.c also leaves class_loadsym set on successful loads, and abstraction pseudo-class names/help directories retain symbols belonging to the retired instance. These are process/instance lifetime mismatches, not parallel ctest execution.

The fix is a guarded build-directory overlay of the two pinned Pd sources, applied from src/juce/pd_loader_fix.cmake. The fetched shared third_party tree stays untouched. Root CMake gets one include to install the overlay; all changes and diagnostics otherwise stay in the delegated Pd paths.

The loaded cache now owns names and compares text; pseudo-class name/help-directory symbols are interned in the immortal main instance; class_loadsym is restored on both success and failure. Recursion diagnostics name the object and current canvas directory. The overlay rejects a changed upstream source instead of silently applying a stale patch.

Verification: MSVC /WX rebuilt the original loader after disabling the overlay (confirmed changed file and successful build); the stress suite crashed with 0xc0000005 again. Restored the CMake file byte-for-byte, rebuilt, and the stress/engine and color-bass Pd suites pass. The stress test also loads both device patches repeatedly in the same process. No shared third_party files changed. Exact-head macOS CI remains the confirmation for the reported allocator-sensitive recursion.
