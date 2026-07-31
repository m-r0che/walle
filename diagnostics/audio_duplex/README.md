# Audio duplex diagnostic

A finite, low-volume hardware diagnostic for the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2.

It configures the ES8311 and I²S path for simultaneous 24 kHz mono PCM16 input/output, discards microphone startup frames, records microphone RMS/peak metrics, plays a 440 Hz tone for two seconds at 20% volume, then mutes the output and prints phase summaries. A pass requires error-free I/O and microphone tone RMS clearly above the quiet baseline.

The test is intentionally finite so a reset cannot leave a continuous tone playing unnoticed.

```bash
. "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C diagnostics/audio_duplex set-target esp32s3
idf.py -C diagnostics/audio_duplex build
idf.py -C diagnostics/audio_duplex -p /dev/cu.usbmodem2101 -b 460800 flash
```
