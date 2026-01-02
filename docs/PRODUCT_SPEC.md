# Loopback Recorder Product Specification

## Background & Intent
- Build a Windows 10/11 CLI tool that records system playback audio (“loopback”) similar to Total Recorder’s core feature set, but purely in user mode.
- Target tool is for user-authorized recordings (tutorials, meetings, etc.); explicitly avoid DRM bypass, kernel drivers, or stealth recording.
- Development stack: Visual Studio 2022, CMake (optionally vcpkg but avoid extra deps unless needed). Start with CLI before GUI.

## Functional Requirements
1. **Loopback capture only** using Windows Core Audio (WASAPI) render loopback path via `IMMDeviceEnumerator`, `IAudioClient`, `IAudioCaptureClient`.
2. **Device handling**
   - Enumerate playback endpoints; default to system default output device.
   - Parameter to pick device by index; optional future support for other selectors.
3. **Recording controls**
   - `--seconds N` to cap duration; Enter key stops early.
   - `--out <path>` to select WAV destination; auto-create directories.
   - Placeholder switch to enable mic mixing in future (default off).
4. **Output format**
   - Write PCM WAV (16-bit or 32-bit float) using the system mix format from `IAudioClient::GetMixFormat`.
   - Ensure WAV header/data chunk sizes are finalized correctly.
5. **CLI options & UX**
   - `--list-devices`, `--device-index`, `--seconds`, `--out`, `--latency-ms`, `--buffer-ms`, `--watchdog-ms`, `--fail-on-glitch`, `--log-file`, etc.
   - Surface clear error messages for invalid arguments.

## Non-Functional Requirements
- **Performance & Stability**
  - Separate capture and writer threads connected by a lock-free (or minimal-lock) ring buffer to prevent disk stalls from blocking capture.
  - Expose configurable latency/ring-buffer sizing to tune for noisy systems.
- **Resilience**
  - Detect device changes/disconnections; exit cleanly with informative logs and stats.
  - Translate every HRESULT into readable text via helper utilities.
- **Logging**
  - Console logging for all major events.
  - Optional file logging enabled via `--log-file`, preserving structured status lines.
- **Maintainability**
  - Modular classes (DeviceEnumerator, LoopbackRecorder, WavWriter, Logger, etc.), each with a clear responsibility.
  - C++20 code style with clear, well-scoped headers/source files.

## Deliverables
1. Project directory with `CMakeLists.txt`, `src/` sources, optional `docs/`, `README.md`, `DELIVERABLES.txt`.
2. Compilable code that records default system audio to WAV without external dependencies beyond Windows SDK.
3. README covering feature overview, build steps (VS2022 + CMake), CLI examples, design notes, and FAQ.
4. `docs/ROADMAP.md` describing evolution: MVP → device selection → pause/resume → segmented output → optional codecs (Opus/AAC).

## Future Considerations
- Implement microphone mix-in path that matches loopback format and blends audio streams safely.
- Add GUI front-end, segmented recordings, advanced logging/metrics, alternate encoders.
- Explore packaging (MSIX/installer) once CLI stabilizes.
