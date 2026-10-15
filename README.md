# KI-M — Local ESP32-S3-BOX-3 AI Companion

Work-in-progress firmware and local voice backend for an ESP32-S3-BOX-3 companion with a pixel avatar, German speech, Qwen TTS, Ollama responses and consent-based memory.

## Included
- `firmware/`: ESP-IDF firmware, pixel assets, Wi-Fi settings, physical BOOT-button-only push-to-talk, hardware mute, OTA and BLE modules. Touchscreen menus remain enabled; touching the main screen does not record.
- `runtime/`: Pipecat voice pipeline, authenticated TLS transport, sentence audio streaming and emotion metadata.
- `runtime/memory_service/`: isolated memory adapter and required upstream Mimir Python source.
- `tests/`, `tools/`, `assets/`: host regression tests, asset generation tools and pixel source art.
- `THIRD-PARTY/`: upstream license notices. Additional dependencies retain their own licenses.

## Status
Source update for firmware **0.5.9** and the matching voice-server diagnostics. The private provisioned app was built with ESP-IDF 6.0.1, installed on a BOX-3 and freshly read back with an exact byte match. Wi-Fi/settings NVS was unchanged by the app-only update. Boot reported 0.5.9, BOOT-only PTT, audio readiness and authenticated backend health. No provisioned binary, credentials or device backup is published here.

The previous firmware's real speech path has been heard working. After the 0.5.9 update, physical confirmation that screen touches do not record and BOOT speech still works remains pending. Host tests do not establish this physical acceptance. OTA code exists, but initialization currently reports unavailable on the tested deployment; do not assume OTA is operational.

Verification: private development firmware suite **76 passed, 5 skipped, 6 subtests passed**, with one pre-existing missing original video-asset test failure. Packaged voice-runtime suite **80 passed**. Actual synthetic German STT/model/TTS turns completed with nonzero audio and terminal `E`, including the public HTTPS route. These results describe the tested deployment/candidate; see [the update notes](docs/UPDATE-0.5.9.md) for the separate sanitized-export checks and remaining work.

## Architecture
The ESP captures deliberate push-to-talk audio. The server runs multilingual Whisper STT, prepares the authored character context and bounded memory recall, streams responses from Ollama, and synthesizes German audio using an external Qwen TTS service. Audio is returned with the existing PCM frame protocol.

Character context is loaded from four private files: `CONSTITUTION.md`, `PERSONA.md`, `SCENARIO.md`, `WORLD.md` under `/run/kim-character`. They are intentionally not published. Supply your own authored character files. Memory writes/deletions use explicit two-step user consent. No autonomous personality rewrite or automatic conversation storage is shipped as an accepted feature.

## Requirements and setup
- ESP32-S3-BOX-3; ESP-IDF **6.0.1**, compatible BSP/managed components from `firmware/main/idf_component.yml` and `firmware/dependencies.lock`.
- External Ollama; the reviewed setup used `gemma4:e2b-it-qat` (verify local model availability).
- External German-capable Qwen TTS endpoint.
- Cached multilingual Whisper model and Python dependencies from `runtime/requirements.txt`.
- Voice Dockerfile uses Python 3.11; memory Dockerfile uses Python 3.12.

Deployment compose files are templates, not ready-to-use credentials. Configure the source, TLS, private-state and model-cache paths (`KIM_SOURCE_DIR`, `KIM_TLS_DIR`, `KIM_PRIVATE_DIR`, `KIM_HF_CACHE`, `KIM_MIMIR_TOKEN_FILE`); inspect all compose fragments before starting. Certificates and authentication tokens must be generated separately. Firmware provisioning is based on `firmware/main/config_local.example.h`; never commit an actual `config_local.h` or provisioned binary.

The voice Dockerfile references NLTK punkt_tab data. Download the required public tokenizer data into the indicated `runtime/tmp-nltk` layout before building; no downloaded dependency/model payload is included in this repository.

### Selected host tests
With pytest and compiler dependencies installed, from the repository root:

```sh
PYTHONPATH=runtime PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python -m pytest -p no:cacheprovider -q \
  runtime/tests/test_stream_emotions.py \
  tests/test_pixel_upgrade.py::test_pixel_avatar_production_c \
  tests/test_pixel_upgrade.py::test_stream_metadata_split_and_legacy_c
```

Additional tests may require managed LVGL/BSP components or additional runtime dependencies. Install them separately; this repository is not an all-in-one offline installer.

## Privacy and safety
No actual Wi-Fi credentials, tokens, TLS keys, private character files, conversation/memory databases, device backups, runtime logs or private firmware binaries are included. Keep hardware mute and consent gates intact. Do not replace the firmware with unverified companion prototypes.

## Licensing
Required third-party code retains its upstream licensing and notices. No new blanket license for the project's original code or character artwork is assigned by this publication.
