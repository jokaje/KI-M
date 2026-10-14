# KI-M — Local ESP32-S3-BOX-3 AI Companion

Work-in-progress firmware and local voice backend for an ESP32-S3-BOX-3 companion with a pixel avatar, German speech, Qwen TTS, Ollama responses and consent-based memory.

## Included
- `firmware/`: ESP-IDF firmware, pixel assets, Wi-Fi settings, touch/BOOT push-to-talk, hardware mute, OTA and BLE modules.
- `runtime/`: Pipecat voice pipeline, authenticated TLS transport, sentence audio streaming and emotion metadata.
- `runtime/memory_service/`: isolated memory adapter and required upstream Mimir Python source.
- `tests/`, `tools/`, `assets/`: host regression tests, asset generation tools and pixel source art.
- `THIRD-PARTY/`: upstream license notices. Additional dependencies retain their own licenses.

## Status
Source snapshot associated with the 0.5.8 pixel firmware candidate. This is not a new production release or a fresh readback of the device. Reported animation stutter, thinking hangs and WLAN/menu usability remain under investigation; source presence is not hardware acceptance.

Six selected tests passed against an isolated export: response emotion parser tests and the production C pixel compositor / stream-metadata parser tests. This does not establish full test-suite, ESP-IDF build or physical acceptance.

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
