# RtAudio fork: code audit and hardening

This document records a memory-safety / correctness / portability audit of
RtAudio and tracks which findings have been addressed in this fork.

Line numbers refer to the source at the time of the audit and drift as the
code changes; use them as a starting point, not an exact address.

## What changed in this fork

### Tooling and CI (`Phase 0`)

* **Unit tests** (`tests/unittest.cpp`): a dependency-free test that runs under
  the dummy API on every platform. Covers the API-name lookup tables (C++ and
  C) and the C-API error-message copy, and is registered in the CMake,
  autotools and meson test suites. (Previously only `apinames` was an actual
  test; the other programs require audio hardware and are never run in CI.)
* **Sanitizers**: a CI job builds with `-fsanitize=address,undefined` and runs
  the test suite.
* **Strict warnings**: a CI job builds the portable (dummy) configuration with
  `-Wall -Wextra -Wpedantic -Werror` on GCC and Clang. CMake gained an opt-in
  `RTAUDIO_WARNINGS_AS_ERRORS` option, and `-Wextra` is now enabled by default
  on GCC/Clang. The previous behaviour of silently injecting `-Werror` into
  Debug builds was removed so downstream Debug builds are not broken by new
  warnings.
* **Static analysis**: a CI job runs `clang-tidy` (config in `.clang-tidy`) and
  `cppcheck`. It is informational for now because the mature backends still
  have outstanding findings.

### Fixed defects (`Phase 1`)

| ID  | Area        | Defect                                                                 | Status |
|-----|-------------|------------------------------------------------------------------------|--------|
| C-1 | C API       | `strncpy(errmsg, text.c_str(), text.size()-1)` — overflow on long messages, `SIZE_MAX` underflow on empty, no NUL on truncation. | Fixed (`rtaudio_c.cpp`, helper in `rtaudio_c_private.h`); regression test added. |
| U-1 | convertBuffer | Left-shift of negative signed integers (UB pre-C++20) in several format conversions. | Fixed by shifting through an unsigned type (value-preserving). |
| U-2 | core        | VLA `char dest[MB_CUR_MAX]` (`MB_CUR_MAX` is runtime in glibc).         | Fixed with compile-time `MB_LEN_MAX`. |
| U-3 | convertBuffer | Stray `if` (should be `else if`) in the SINT8 output chain.           | Fixed. |
| C-2 | CoreAudio   | `closeStream()` dereferenced a possibly-NULL `handle` during cond-var teardown. | Fixed (guarded). |
| M-2 | CoreAudio   | `probeDeviceInfo()` used unchecked `malloc` and ignored `CFStringGetCString` failure before `strlen`. | Fixed. |
| M-3 | JACK        | `jack_get_ports()` result indexed without a NULL check.                | Fixed. |
| H-1 | PulseAudio  | Handle (and its streams / cond var) leaked on open failure due to a shadowed `pah` and an error guard that never fired. | Fixed. |

### Fixed defects (`Phase 2`)

These were addressed in a second pass. They are backend changes that are
compile-verified by CI (CoreAudio on macOS, WASAPI/DirectSound via the MinGW
cross-builds) but, lacking the target hardware here, were **not** runtime-tested
— review accordingly.

| ID  | Area        | Defect                                                                 | Status |
|-----|-------------|------------------------------------------------------------------------|--------|
| C-3 | CoreAudio   | `streamDisconnectListener` called `closeStream()` directly on the property-listener thread (removes listeners from within a listener → deadlock; races the audio callback). | Fixed: spawns a detached teardown thread, mirroring JACK. |
| C-5 | WASAPI      | `WasapiResampler` ctor ignored COM HRESULTs → NULL deref on the audio thread if the resampler MFT is unavailable. | Fixed: guards the interface pointers, adds `isValid()`, and `Convert()` passes through when the transform is absent. |
| C-6 | WASAPI      | `WasapiResampler::Convert()` copied the resampler output without clamping to the caller's buffer capacity. | Fixed: clamps the copy to `outputBufferSize`. |
| C-4 | DirectSound | Device/buffer COM objects leaked on open failures that occurred after creation but before they were stored in the handle. | Fixed: the error path releases the still-owned objects. |
| H-5a| WASAPI      | SPSC ring-buffer indices (`inIndex_`/`outIndex_`) were plain `unsigned int` shared across threads → data race. | Fixed: now `std::atomic<unsigned int>`. |

### Fixed defects (`Phase 3`)

Another backend pass (compile-verified by CI; not runtime-tested here).

| ID  | Area  | Defect                                                                          | Status |
|-----|-------|---------------------------------------------------------------------------------|--------|
| H-5 | core  | `stream_.state` was a plain enum read by the realtime callback thread and written by the control thread. | Fixed: now `std::atomic<StreamState>` (all ~180 access sites are plain load/store, so the type change is transparent). |
| H-4 | JACK  | `closeStream()` only called `jack_deactivate` when `state == RUNNING`, leaving a window where the realtime xrun callback runs against freed ports. | Fixed: always deactivates before unregistering ports / freeing the handle. |
| H-2 | JACK  | The duplex INPUT error path tore down the client/handle shared with the already-open OUTPUT pass. | Fixed: the error path only frees the handle when this call allocated it. |
| H-3 | ASIO  | The process-global `asioCallbackInfo` was never cleared in `closeStream()`, so a late `sampleRateChanged`/`asioMessages`/`bufferSwitch` dereferenced freed state. | Fixed: cleared on close and NULL-checked in all three driver callbacks. |
| M-1 | OSS   | `OssHandle::id[]` used `0` as the "unused" sentinel, but `0` is a valid descriptor → fd leak / mishandling. | Fixed: uses `-1`; close checks are now `>= 0`. |

### Tests and findings (`Phase 4`)

* **Conversion tests** (`tests/convtest.cpp`): a dependency-free test that
  reaches the protected `convertBuffer`/`byteSwapBuffer` routines through a
  minimal `RtApi` subclass (no library change). Covers byte-swapping, the
  formerly-UB signed left-shift conversions (with known negative-value results),
  format round-trips, and channel mapping. This is the runtime validation of the
  Phase 1 `convertBuffer` UB fix that was previously only review-verified — it
  passes under `-fsanitize=address,undefined`. `tests/unittest.cpp` also gained
  a C-API instance lifecycle smoke test.
* **M-7 WASAPI ring-buffer narrowing** — the wrap-size computation
  (`inIndex_ + bufferSize - bufferSize_`) was done in unsigned then narrowed to
  `int` (implementation-defined). Now computed in signed 64-bit and clamped.
* **OSS duplex trigger** — `SNDCTL_DSP_SETTRIGGER` ioctl results are now checked,
  and `handle->triggered` is only set when both succeed (previously it was set
  unconditionally even if the input/output sync failed).
* **DirectSound / ASIO `CreateEvent`** — the drain/condition event handle is now
  checked; a failed `CreateEvent` errors out of `probeDeviceOpen` instead of
  leaving a NULL handle that breaks drain signaling.

## Known / deferred findings

These remain open. Severity: 🔴 critical, 🟠 high, 🟡 medium.

* 🟡 **ASIO single-instance guard** (`streamOpen`) is a plain `bool` with a
  check-then-set race if two `RtApiAsio` instances open concurrently. ASIO is
  effectively single-driver, so this is low-impact; left as-is.
* 🟡 Remaining ignored return codes (ALSA `snd_pcm_drop`, some
  `CoCreateInstance` call sites) and the CoreAudio/JACK `pthread_cond_wait`
  drain handshakes that are not predicate loops (changing them alters wait
  semantics, so they need on-hardware validation).

## Suggested next steps

1. Runtime-validate the Phase 2/3 backend fixes on real hardware (CoreAudio
   disconnect, WASAPI resampler + atomics, DirectSound, JACK, ASIO, OSS).
2. Drive `clang-tidy`/`cppcheck` toward zero findings per backend, then make the
   static-analysis job blocking.
3. Add a native MSVC CI job (the current Windows coverage is MinGW cross-builds
   only) and consider a coverage report for the portable code.
