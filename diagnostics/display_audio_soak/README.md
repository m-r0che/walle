# Display/audio soak diagnostic

Runs the production face renderer while continuously reading and writing 24 kHz mono PCM16 audio for 30 seconds. Speaker output is explicitly muted and TX contains only silence.

The diagnostic reports audio I/O errors and latency, microphone RMS/peak, elapsed timing, face render telemetry, and minimum free internal/PSRAM heap. It prints `DISPLAY_AUDIO_SOAK_RESULT=PASS` only when both audio streams complete on time without errors.

```bash
. "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C diagnostics/display_audio_soak set-target esp32s3
idf.py -C diagnostics/display_audio_soak build
idf.py -C diagnostics/display_audio_soak -p /dev/cu.usbmodem2101 -b 460800 flash
```
