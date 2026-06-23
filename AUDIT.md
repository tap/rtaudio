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

## Known / deferred findings

These are real but remain deliberately **not** changed because the correct fix
changes cross-cutting runtime behaviour and cannot be validated without the
target hardware. Recorded for a follow-up that can test on-device.
Severity: 🔴 critical, 🟠 high, 🟡 medium.

* 🟠 **WASAPI/other realtime threads read `stream_.state` without
  synchronization** (`wasapiThread`, ~5911/6219). `stream_.state` is a plain
  enum shared between the audio thread and control thread. Making it atomic is
  a cross-cutting change (~180 use sites across every backend) and was left for
  a dedicated pass; the WASAPI ring-buffer indices (the more concrete race) are
  now atomic (H-5a).
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
