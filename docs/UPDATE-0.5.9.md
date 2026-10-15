# 0.5.9 source update — button-only listening

## Included changes

- Physical BOOT-button-only hold-to-talk. Ordinary touchscreen press/release/press-loss no longer starts, submits or cancels voice. Touchscreen settings and wake/release handling remain; explicitly opening the menu still cancels a turn for privacy.
- Updated on-screen hint and boot banner; app version 0.5.9.
- Preserve prior stream playback core-pinning and yielding fixes present in the installed firmware. These measures are not a demonstrated root-cause fix for all interrupted replies.
- Include device-state heartbeat and asynchronous character-state polling modules already present in the installed firmware source. They are client-side integration scaffolding, NOT proof that a second/background model is running. The current published/live voice runtime has no autonomous background-model worker. In particular, its transport does not implement `/v1/device-state` or `/v1/state`: the client requires HTTP 204 for the heartbeat, retries when it is unavailable, and does not progress to character-state polling while that heartbeat remains due. Server routes and end-to-end privacy/state behavior must be implemented and verified before calling this integration operational.
- Content-free server processor-error diagnostics: original exception class/source location and aggregate input sample count, RMS, peak, nonzero and clipping counts. No exception messages, transcripts or audio are logged by these additions. Cover both regular Pipecat processor failures and the asynchronous TTS-worker catch path.
- Regression tests compile the actual screen callback and exercise failed STT/TTS through the actual Pipecat scheduler.

## Evidence and limitations

The private provisioned firmware was built, app-only flashed, read back exactly and booted with working authenticated health. Settings/WLAN bytes matched before and after the app write. The device-level button/touch behavior after this update still needs attended confirmation. The prior firmware's speech path was heard working by the user.

Development tests: firmware 76 passed, 5 skipped, 6 subtests passed, one pre-existing missing retired original-video asset failure; packaged voice runtime 80 passed. Real synthetic German input produced nonzero streamed reply audio and a valid terminal E through the deployed public TLS endpoint. An empty/near-silent short device turn produced an explicit no-speech-recognized error; this supports, but does not prove, accidental touchscreen activation as its cause.

Sanitized-export checks: **14 selected firmware tests passed**; the separate real-LVGL composition test cannot run in a clean export until the external managed LVGL dependency is installed. The **complete exported runtime suite passed all 80 tests** in the runtime dependency image. Both live-probe helpers imported correctly with example-only private-directory and TLS-server-name configuration. The file-level privacy scan compared against private provisioning/network values without exposing them and found no matches; personal local-path defaults in inherited helper scripts were replaced with configurable portable paths.

Public-source checks can be run without credentials. The firmware callback and BOOT tests require a C compiler and pytest; the complete runtime suite additionally requires the runtime Python dependencies. Some firmware/UI tests require external managed LVGL/BSP dependencies and intentionally unpublished original video sources. This is source publication, not a claim that a clean clone is a complete offline installer.

## Remaining project work

1. **Device acceptance and reliability:** attended screen-no-recording/BOOT-speech checks, repeated long/short turns, mute/menu/sleep/wake, known-WLAN reconnect and recovery after network/server outages. Occasional TLS-connect timeouts have been observed; do not label the system fully stable yet.
2. **Clear failure UX:** distinguish no speech, cancellation and transport/model/TTS failures instead of displaying the same generic "Reply interrupted" message.
3. **OTA readiness:** the module exists, but the tested device reports OTA unavailable/invalid state. Investigate actual partition/provisioning/trial requirements and validate signed update, confirmation and rollback end-to-end.
4. **Memory acceptance:** verify spoken explicit consent, exact-record readback, recall, deletion, restart persistence and isolation on a disposable test namespace before broad daily-use claims. Code integration is not physical acceptance.
5. **Two-model companion:** integrate the separately prepared background-model candidate into the deployed runtime only after review. Verify state/privacy leases end-to-end, foreground priority, stale-result rejection and persistence. No autonomous personality development is claimed for the current deployment.
6. **Natural conversation (optional):** hands-free endpointing, acoustic echo cancellation, speaker/mic coexistence and real barge-in need physical qualification. The current requested mode remains BOOT-only PTT; do not silently enable listening features.
7. **Productization:** reproducible clean-clone build, CI, complete setup instructions and dependency provisioning, secure device onboarding, measured audible latency/avatar timing, release packaging and recovery documentation. No new hardware purchase is necessary to prove the BOX-3 interaction.

## Deliberately excluded

No real provisioning header, Wi-Fi profiles/passwords, device/backend tokens, certificates/private keys, private character files, private memory databases, conversations, device dumps, local deployment scripts, runtime logs or provisioned firmware binaries. Existing sanitized Compose templates remain templates; private live deployment overrides are not copied into this repository. Supply local credentials and your own authored character separately.
