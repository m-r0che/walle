# Display transfer ownership diagnostic

Finite CO5300 QSPI diagnostic for the intermittent physical blank-screen regression.
It bypasses LVGL, uses one 20-row internal DMA buffer, and waits for
`on_color_trans_done` before reusing that buffer.

The test:

1. initializes the panel at fixed 45% brightness;
2. draws a complete static pattern in sequential strips;
3. submits 1,000 moving-strip updates with transfer-completion backpressure and a 20 ms pacing gap;
4. redraws and holds the static pattern;
5. reports submitted/completed counts, timeouts, errors, and transfer latency.

A serial `PASS` validates the callback/ownership seam but does not replace physical confirmation that the cyan-bordered face-like pattern remains visible. Physical testing found an additional panel limit invisible to completion telemetry: 500 updates at a 5 ms gap remained visible, 1,000 at 5 ms produced a black panel despite every callback completing, and 1,000 at 20 ms remained visible. The production driver therefore coalesces the complete 320×220 face into one transfer per frame instead of sending eleven 20-row strips.

```bash
. "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
idf.py -C diagnostics/display_transfer build
idf.py -C diagnostics/display_transfer -p /dev/cu.usbmodem2101 -b 460800 flash
```

After testing, restore the known-visible production image from `firmware/build`
(or rebuild and verify its documented SHA-256) before continuing normal use.
