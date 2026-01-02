# ROADMAP

## Phase 0: MVP (Now)
- Support default playback device WASAPI loopback recording only.
- CLI parameters for output path, duration, logging, latency/buffer tuning.
- WAV writing, capture/writer thread split, structured logging and error handling.

## Phase 1: Device Management
- Harden `--list-devices` / `--device-index` UX (already present) and auto-refresh after device changes.
- Surface disconnect prompts and allow quick re-selection or retry.
- Remember last-used device per user profile.

## Phase 2: Control Enhancements
- Add pause/resume commands (state machine + console hotkeys).
- Rolling file output by duration or size with graceful WAV finalization.
- Safe-stop hooks (flush buffers, checksum manifest).

## Phase 3: Encoding & Audio Processing
- Optional microphone mixing with latency alignment.
- Additional encoders (Opus/AAC via Media Foundation) alongside WAV.
- Pluggable DSP modules (limiter, normalization, noise reduction).

## Phase 4: Visualization & GUI
- JSON/IPC control endpoints for GUI shells.
- Lightweight WinUI or tray client for non-CLI users.

## Phase 5: Automation & Monitoring
- Log rotation/upload, Prometheus-friendly stats, watchdog alerts.
- Pre-flight checks (exclusive-mode conflicts, permissions, disk space).