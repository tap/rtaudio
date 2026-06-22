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

## Known / deferred findings

These are real but were deliberately **not** changed in this pass because the
correct fix changes runtime behaviour (threading model, lock-free state) and
cannot be validated without the target hardware. They are recorded here for a
follow-up that can test on-device. Severity: 🔴 critical, 🟠 high, 🟡 medium.

* 🔴 **CoreAudio disconnect listener calls `closeStream()` on a CoreAudio
  thread** (`streamDisconnectListener`, ~RtAudio.cpp:1104). Frees buffers and
  the handle with no coordination with the running audio callback → race /
  use-after-free on device disconnect. JACK and ASIO instead spawn a teardown
  thread; CoreAudio should do the same.
* 🔴 **WASAPI resampler ignores all HRESULTs** (`WasapiResampler` ctor,
  ~RtAudio.cpp:4626) → NULL deref on the audio thread if the resampler MFT is
  unavailable. `Convert()` (~4707) also ignores `Lock` HRESULTs and does not
  clamp the output copy length to the destination capacity (possible overflow
  of `stream_.deviceBuffer`).
* 🔴 **DirectSound device/buffer leak on every failed open**
  (`probeDeviceOpen`, ~RtAudio.cpp:6939). The COM objects live in locals and
  are only copied into `handle` at the end, so the `error:` cleanup (which
  releases via `handle`) is skipped.
* 🟠 **WASAPI realtime thread accesses `stream_.state` and the SPSC ring-buffer
  indices without atomics** (`wasapiThread`, ~5911/6219; indices ~4522/4584).
  `stream_.state` is a plain enum and the ring indices are plain `unsigned
  int`, shared with the control thread → data races / missed stop signal. Make
  `stream_.state` and the indices `std::atomic` with acquire/release.
* 🟠 **JACK duplex-input error path tears down the shared output client**
  (~RtAudio.cpp:2996) and **`jackXrun` can run after the handle is freed** when
  `closeStream()` deletes without `jack_deactivate` first (~2965).
* 🟠 **ASIO process-global state** (`drivers`, `asioCallbackInfo`, `streamOpen`)
  is unsynchronized and never cleared in `closeStream()` → stale-pointer
  deref from `asioMessages`/`sampleRateChanged`, and a second instance corrupts
  the first.
* 🟡 **OSS uses fd `0` as the "unused" sentinel** (`OssHandle::id[]`), but `0`
  is a valid descriptor → fd leak / mishandling. Use `-1`.
* 🟡 Various ignored return codes (ALSA `snd_pcm_drop`, OSS `SNDCTL_*` ioctls,
  WASAPI/DS `CreateEvent`/`CoCreateInstance`), narrowing in the WASAPI ring
  buffer, and `pthread_cond_wait` drain handshakes that are not predicate loops.

## Suggested next steps

1. Address the 🔴 items on real hardware (CoreAudio disconnect threading,
   WASAPI resampler guards + atomics, DirectSound leak).
2. Drive `clang-tidy`/`cppcheck` toward zero findings per backend, then make the
   static-analysis job blocking.
3. Add a native MSVC CI job (the current Windows coverage is MinGW cross-builds
   only) and consider a coverage report for the portable code.
