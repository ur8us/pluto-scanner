# Agent Notes

## Project Shape

`pluto-scanner` is a C backend plus a single-page HTML frontend for ADALM-Pluto.

- `main.c` serves HTTP on `127.0.0.1:8080` by default, exposes JSON API endpoints, controls Pluto through `libiio`, computes FFT rows, and streams waterfall data over SSE. `--port` overrides the incoming HTTP port; `--bind`/`--listen` may expose the UI on another address for trusted LAN use only.
- `index.html` owns the spectrum/waterfall canvases, Pluto controls, zoom/pan, Go To, rulers, bands, and markers.
- `pluto-scanner.conf` is local runtime state and should stay untracked.
- `images/` contains README screenshots captured from the live browser UI.
- `PLUTO.MD` reference for ADALM-Pluto hardware (libiio API, clock control, SSH).
- `SPEC.MD` full API specification (endpoints, status fields, SSE row format, controls).
- `SPECTRUM_CALC.MD` spectrum math reference (FFT, CIC, filter, level normalization formulas).

## Pluto Backend

The scanner does not use a hardware scan API. It emulates scan mode on the host:

1. Build air-frequency hop centers.
2. Convert air frequency to Pluto receiver frequency using converter math.
3. Write AD936x RX LO through `ad9361-phy` / `altvoltage0`.
4. Discard two stale buffers.
5. Refill one useful RX buffer from `cf-ad9361-lpc`.
6. Decode I/Q, FFT, crop centered passband bins, and publish one SSE row.

The measured scan/hop default is:

```text
sample_rate      = 61440000
rf_bandwidth     = 20000000
fft_size         = 1024
buffer_samples   = 1024
kernel_buffers   = 1
discard_count    = 2
gain_mode        = manual
hardwaregain_db  = 20
```

Single-frequency continuous capture uses `kernel_buffers = 4`. Do not reduce
that path to one IIO kernel block: CIC state spans refills, while I/Q extraction,
float conversion, and queue copying delay the next refill. Scan/hop mode keeps
one block because continuity between retuned centers is irrelevant and stale
latency is undesirable.

The app now supports the unofficial extended Pluto receiver range of `70 MHz..6 GHz`. Keep frontend frequencies as air/signal frequencies and enforce the receiver limits after converter conversion.

The scanner must not require Pluto reflashing. It is expected to work with the
original Analog Devices stock firmware and DATV firmware variants that expose
the normal libiio devices and AD936x attributes.

The HTTP backend is local-only by default. Do not add implicit remote exposure,
authentication placeholders, multi-user controls, or per-user session behavior
unless the project scope changes explicitly.

## Required Operating Modes

Agents must preserve these two Pluto operating modes. Do not port Fobos assumptions back into this project.

1. **Scan/hop mode for smaller zoom factors and wide spans.**
   - Use this mode when the visible span cannot be covered by one Pluto passband.
   - Prefer the highest stable Pluto sample rate for this mode, because scan/hop captures short buffers at each retuned center rather than transferring one uninterrupted long USB stream.
   - Auto-select Pluto RF transport settings; the UI may display sample rate, RF bandwidth, and passband usage, but agents should not make them ordinary user-tunable controls again.
   - Choose hop spacing from the usable RF bandwidth and passband usage so stitched sub-bands avoid obvious edge-response degradation.
   - Use the shortest FFT size that still gives useful screen resolution and fast line cadence.
   - Treat a waterfall line as a display product: FFT bins from hop slots should map predictably into points of the published line, with no hidden Fobos frequency-response compensation.

2. **Single-frequency mode for high zoom factors and narrow spans.**
   - Use this mode when one Pluto receiver center can cover the visible span.
   - Auto-select sample rate, RF bandwidth, FFT size, and CIC decimation from the visible-span resolution product. Do not change those resolution coefficients just because the requested minimum waterfall rate changes.
   - The minimum waterfall-rate control is implemented by integer overlapping FFT windows on the decimated CIC stream. It may reduce `decim_hop`, but it must not reduce FFT size, CIC decimation, hardware sample rate, RF bandwidth, source span, or visible raw-bin density for the same view.
   - The maximum waterfall-rate control may throttle completed FFT lines. In CIC mode it must never throttle by dropping raw buffers before the decimator.
   - Keep the scan/hop to single-frequency transition invisible to the user; Go To, zoom, pan, rulers, and waterfall history should continue to behave as one continuous spectrum tool.
   - Preserve persistent waterfall history when zooming or moving through frequencies. This is a known SDR UI principle, but this project should improve it by keeping historical rows useful across large zoom and frequency changes instead of clearing the display.
   - **Each published waterfall line must come from exactly one FFT.** Do not average multiple FFTs into a single line. In CIC mode, every raw sample must enter the decimator exactly once in order: no sample loss between buffers, no duplicate insertion, and no silent overwrite of one FFT result by another within the same buffer. Adjacent FFT windows may intentionally share already-decimated samples when `overlap_factor > 1`.
   - Decimated single-stream mode must not rate-drop or queue-drop raw buffers
     before the CIC path. Raw gaps corrupt the stateful decimator and can widen
     signals in the waterfall. Throttle CIC mode only after a complete FFT line
     has been produced, or by the final publish-clock guard.
   - Decimated single-stream mode should request one line-sized Pluto/IIO
     refill whenever practical: `async_samples = decim_hop * decim`. This
     keeps the common `FFT=65536 x2` case as one hardware refill per displayed
     line. Very large line sizes may be capped by `SINGLE_DECIM_ASYNC_MAX_LEN`
     and must remain visible in backend logs as `async < line samples`.
   - Single-frequency continuous capture must use multiple queued IIO kernel
     buffers (`PLUTO_CONTINUOUS_KERNEL_BUFFERS = 4`). The scan/hop setting of
     one kernel buffer is not valid for stateful CIC streaming.
   - On compatible high-zoom view changes, bounded historical raw samples may
     be processed as `preview=1` rows before the first live `preview=0` row.
     Very fine frequency resolution needs FFTs built from many seconds of
     samples; traditional SDR programs can make the user wait several seconds
     after a resolution change before a new waterfall line appears. Preview
     processing reuses compatible memory-held samples for the first rows, must
     use the same CIC/Hann/FFT path, must be marked in SSE metadata, and must
     reset all CIC state before live acquisition resumes. The backend may send
     multiple preview rows immediately, but the frontend must release them at
     `preview_interval_ms` while the independent live stream fills; never join
     pre-restart cache samples to post-restart live samples in an FFT.

## FFT, CIC, and RF Planning Rules

The planner publishes an exact display product. Every SSE waterfall row must
contain exactly `display_bins` processed output values, one output bin per
screen pixel. Keep raw FFT/CIC bin counts in metadata as `raw_source_bins` or
`raw_line_bins` for debugging, but do not expose oversized raw rows as the
frontend data model. Current SSE rows compress the display bytes as
`encoding:"u8b64"` with `db` containing base64 packed `uint8` values; keep
frontend compatibility with legacy `d:[...]` rows when editing this path.

RF bandwidth must be strictly lower than sample rate for all Pluto profiles:

```text
rf_bandwidth_hz < sample_rate_hz
```

Scan/hop mode:

- Hardware sample rate is auto-forced to `61440000` samples/s.
- Hardware RF bandwidth is auto-forced to `20000000` Hz.
- Passband usage is auto-forced to `0.85`.
- Hop step is `rf_bandwidth * passband_usage`, currently `17000000` Hz.
- FFT size is `clamp_pow2(display_bins * 61440000 / visible_span_hz, 1024, 8192)`.
- CIC decimation is always `1`.
- Raw scan bins should cover the display, then `publish_scan_line()` reduces
  them to exactly `display_bins` values by peak-per-pixel aggregation.
- Raw scan line rate is estimated from the measured hop time:
  `1000 / (hop_ms(scan_fft) * hop_count)`.

Single-frequency mode:

- Use this mode when the visible span fits in one usable Pluto passband.
- Use conservative displayed-passband hardware profiles:
  `(4e6,2e6)`, `(8e6,4e6)`, `(16e6,8e6)`, `(20e6,10e6)`,
  `(30.72e6,15e6)`, `(61.44e6,20e6)`.
- Select the smallest profile whose `min(sample_rate, rf_bandwidth)` covers
  `visible_span_hz + 2 * zero_if_guard_hz`.
- Use the resolution product
  `display_bins * hardware_sample_rate_hz / visible_span_hz`.
- Choose `decim = clamp_pow2(ceil(product / 65536), 1, 4096)`, then reduce it
  while `hardware_sample_rate_hz / decim` would no longer cover the visible
  span with a small margin. Do not force the zero-IF guard into CIC decimation
  if doing so would reduce the visible raw-bin density below one bin/pixel.
- Choose `fft = clamp_pow2(ceil(product / decim), 2048, 65536)`.
- Raw single/CIC bins should cover the display, then the published row is
  reduced to exactly `display_bins`.
- CIC paths use the measured host throughput estimate `1850000` raw samples/s.
  Non-CIC single paths use `2100000` raw samples/s.
- Max waterfall rate throttling happens after planning with a `0.85` guard.

Frontend RF display:

- API responses expose both hardware and active values.
- `hardware_samplerate`, `hardware_rf_bandwidth`, and `hardware_bw_ratio` are
  the physical Pluto settings.
- `active_samplerate`, `active_rf_bandwidth`, and `active_bw_ratio` are the
  current zoom-mode values shown by the disabled frontend controls.

## Output Level Normalization

The scanner must preserve the same coherent signal calibration when FFT size or
CIC decimation changes. Waterfall white-noise presentation is separately
normalized for resolution bandwidth.

- FFT windowing uses a Hann window.
- Magnitudes are normalized by coherent window gain: `1 / sum(window)`.
- Do not multiply level by FFT size, CIC factor, or a Fobos compensation curve.
- CIC decimation has a three-stage CIC response implemented as bounded moving
  sums, one length-`decim` delay ring per stage. Its DC gain is normalized by
  dividing the decimated output by `decim^3`. Do not implement CIC as
  unbounded floating-point integrators; long-running captures lose precision.
- Per-bin CIC droop is compensated with the inverse normalized CIC response:
  `abs(sin(pi * f * decim / raw_sample_rate) /
  (decim * sin(pi * f / raw_sample_rate))) ^ 3`.
- Clamp the inverse CIC compensation to `8x` and floor the modeled CIC gain at
  `0.05` so edge bins cannot explode if the response approaches a null.
- Waterfall byte presentation additionally normalizes white-noise density with
  `sqrt(90000 / hann_enbw_hz)`, where
  `hann_enbw_hz = fft_samplerate * sum(window^2) / sum(window)^2`. It is
  display-only: do not feed it into CIC state or coherent FFT amplitude.
- Peak-per-pixel reduction uses a Rayleigh median correction for the actual
  raw-bin count in a pixel. Preserve peak signal sensitivity while preventing
  low-zoom multi-bin reduction from making the noise background artificially
  brighter. See `SPECTRUM_CALC.MD` for the exact formula and the `1024x` cap.
- Validate changes with `tools/fft_level_normalization_check.py`.

## Runtime Stability Expectations

Pluto scan mode is sensitive to rapid retune/restart loops. The frontend intentionally coalesces ordinary zoom/pan view updates before asking the backend to retune. Preserve these behaviors:

- Shift-wheel, zoom buttons, and pan controls should update the canvas immediately but should not restart the SDR for every small step.
- Non-animated Go To may retune immediately to the final target.
- Animated Go To requests a backend view at each animation step and waits for a
  matching row, so every step shows true current-resolution data rather than a
  stretched preview of the previous lower-resolution row.
- When stopping or restarting a scan, libiio can print `ERROR: READ ... -9` from canceled buffer operations. Treat those as expected cancellation-side noise unless followed by `Closing SDR device after I/O error` or `Device reconnected`.
- A small number of `pluto_sdr_read_async failed: -1; retrying RX buffer` lines can occur during stress tests. That is acceptable if the retry recovers and the device is not closed.

If a UI change reintroduces repeated backend restarts, capture a before/after
backend trace and compare retune/restart counts.

## Pluto Disconnect and Reconnect Handling

The scanner must gracefully handle sudden Pluto disconnection (USB cable unplugged, Pluto crash, network timeout) and automatically resume scanning when the device reappears.

### Disconnect Detection

- During a scan, if `pluto_sdr_read_async()` or other IIO operations return a hard error (not `-9` from buffer cancel), set `device_error = 1` in the scan thread.
- When the scan thread exits with `device_error` set, log `Closing SDR device after I/O error; will poll for reconnect`, then call `close_device()` to destroy the libiio context and set `g_dev = NULL`.
- Treat `ERROR: READ ... -9` from canceled buffer operations as expected cancellation noise. Do not close the device for those.
- A small number of `pluto_sdr_read_async failed: -1; retrying RX buffer` lines can occur under stress. Only flag a disconnect if the retry does not recover, or if `pluto_sdr_set_frequency()` returns a hard error.

### Reconnection Poll Loop

- In the main event loop (once per second `select` timeout), call `poll_device_reconnect()`.
- `poll_device_reconnect()` must not poll more often than once every 5 seconds.
- Only poll when `g_dev == NULL` and `g_scanning == 0` (the device is known to be disconnected and no scan is running).
- The poll calls `open_first_device(0)` which creates a new libiio context and opens the Pluto.
- On success, log `Device reconnected`. Do NOT log a reconnection if `g_dev` is already non-NULL.

### Auto-Restart Scan on Reconnect

- The backend tracks a flag `g_auto_restart_on_reconnect` that is set to `1` whenever a scan is running and a device error forces the scan to stop.
- After `poll_device_reconnect()` successfully opens the device, check `g_auto_restart_on_reconnect`:
  - If `1`, call `start_scan()` to automatically resume scanning with the current frontend frequency/view settings. Log `Auto-restarting scan after reconnect`.
  - If `0`, just leave the device open and idle, waiting for a manual Start from the frontend.
- The frontend heartbeat status line should reflect the auto-reconnect state (`reconnecting...`, `scanning`).
- The frontend does NOT need to send a Start request; the backend handles it transparently.

### Safety and Debouncing

- After a reconnect and scan restart, suppress further auto-reconnect for at least 3 seconds to avoid rapid retry loops if the device is flapping.
- If the scan fails again within 3 seconds of an auto-restart, clear `g_auto_restart_on_reconnect` so the backend does not loop forever. The frontend can still manually Start.
- The poll interval (5 seconds) already prevents tight retry loops for the open step.

### Diagnostics

- Log all disconnect and reconnect events at `[SDR]` level with timestamps.
- The trace log is checked for:
  - `Closing SDR device after I/O error` — disconnect logged correctly.
  - `Device reconnected` — reconnection detected.
  - `Auto-restarting scan after reconnect` — scan resumed.
  - Absence of repeated reconnect/restart cycles without frontend interaction.

## Removed Legacy Features

Do not reintroduce:

- Legacy LNA/VGA controls as visible UI.
- Legacy direct sampling.
- Legacy external clock control.
- Frequency-response compensation UI/API.
- Legacy hardware-specific build or runtime dependencies.

## Frontend Behavior

Preserve:

- SSE row protocol.
- Spectrum and waterfall display.
- Shift-wheel zoom.
- Drag pan.
- Go To dialog.
- Ctrl-drag rulers.
- Markers and band overlays.
- Browser heartbeat showing disconnected/idle/scanning.

The Pluto UI exposes sample rate, RF bandwidth, passband usage, gain mode, and hardware gain. Preserve the visible `Shown: start - end MHz, Span: range MHz` status-line text. Do not re-add the old separate Range badge, the `FW:` indicator, RSSI/Peak readout, frontend Device/serial indicator, or passive `Pluto Input` group. Backend logs should print Pluto software version and serial once the hardware opens.

## Documentation Standard

Document new or changed backend functions with Doxygen comments:

```c
/**
 * @brief One-sentence purpose.
 *
 * Additional notes for planner/math functions should include units and
 * invariants, such as whether values are hertz, bins, or lines/s.
 *
 * @param name Meaning and unit.
 * @return Meaning and unit, or success/error contract.
 */
```

Document new or changed frontend functions with JSDoc comments:

```js
/**
 * One-sentence purpose.
 *
 * @param {number} value Meaning and unit.
 * @returns {number} Meaning and unit.
 */
```

For coefficient, FFT, CIC, waterfall, and RF-profile code, include enough
machine-readable detail that editors can expose the formulas and units without
opening `SPECTRUM_CALC.MD`.

## Pluto Coefficient Planning

Agents changing scan/hop or single-frequency behavior must preserve these coefficient rules.

Scan/hop mode is used when the visible span needs more than one Pluto passband:

```text
scan_sr_hz = 61.44e6
scan_rf_bw_hz = 20e6
scan_pb = 0.85
scan_step_hz = scan_rf_bw_hz * scan_pb
hops = ceil(visible_span_hz / scan_step_hz)
mode = scan when hops > 1
scan_fft = clamp_pow2(display_bins * scan_sr_hz / visible_span_hz, 1024, 8192)
scan_async_samples = max(1024, scan_fft)
raw_scan_bins ~= scan_fft * visible_span_hz / scan_sr_hz
published_bins = display_bins
```

Single-frequency mode is used when one receiver passband covers the visible span:

```text
profiles = (4e6,2e6), (8e6,4e6), (16e6,8e6), (20e6,10e6), (30.72e6,15e6), (61.44e6,20e6)
zero_if_guard_hz = 50000
profile = smallest profile where min(sample_rate, rf_bandwidth) >= visible_span_hz + 2 * zero_if_guard_hz
required_product = ceil(display_bins * hardware_sample_rate_hz / visible_span_hz)
decim = clamp_pow2(ceil(required_product / 65536), 1, 4096)
while decim > 1 and hardware_sample_rate_hz / decim < visible_span_hz * 1.05:
  decim = decim / 2
fft = clamp_pow2(ceil(required_product / decim), 2048, 65536)
decim_async_min_samples = 8192
decim_async_max_samples = 262144
nonoverlap_cic_lps = min(hardware_sample_rate_hz, 1.85e6) / (fft * decim)
target_lps = min(min_rate_lps, rate_limit_lps) when decim > 1 and min_rate_lps > 0
required_overlap = ceil(target_lps / nonoverlap_cic_lps)
overlap_factor = next_pow2(max(1, required_overlap))
overlap_factor = min(overlap_factor, fft)
decim_hop = fft / overlap_factor when decim > 1 and min_rate_lps > 0
decim_hop = fft otherwise
line_samples = decim_hop * decim
single_async_samples = min(line_samples, decim_async_max_samples) when decim > 1
single_async_samples = fft when decim == 1
fft_sample_rate_hz = hardware_sample_rate_hz / decim
source_span_hz = min(hardware_rf_bandwidth_hz, fft_sample_rate_hz)
second_if_hz = min(visible_span_hz / 2 + zero_if_guard_hz, max(0, (source_span_hz - visible_span_hz) / 2))
raw_source_bins ~= fft * source_span_hz / fft_sample_rate_hz
visible_raw_bins ~= raw_source_bins * visible_span_hz / source_span_hz
published_bins = display_bins
```

Line-rate estimates are empirical host-side estimates:

```text
scan_hop_ms(1024) = 3.4
scan_hop_ms(2048) = 4.8
scan_hop_ms(4096) = 6.0
scan_hop_ms(8192) = 8.9
raw_scan_lps = 1000 / (scan_hop_ms(scan_async_samples) * hops)
raw_single_lps = min(hardware_sample_rate_hz, 2.1e6) / fft
raw_cic_lps = min(hardware_sample_rate_hz, 1.85e6) / (decim_hop * decim)
true_line_rate = min(hardware_sample_rate_hz, cic_or_single_stream_sps) / (fft * decim)
```

`raw_line_rate` may include minimum-rate overlap boosting. `true_line_rate`
must remain the base FFT cadence before that boost and is what the frontend
shows in the FFT status text.

Line-rate throttling is per-mode:
- **Scan mode**: if the estimated scan line rate exceeds the maximum rate,
  drop complete scan cycles before FFT with the `0.85` guard.
- **Single non-CIC mode**: if the estimated raw FFT line rate exceeds the
  maximum rate, drop whole raw FFT buffers before FFT with the `0.85` guard.
  This is allowed because no stateful CIC filter spans those buffers.
- **Single CIC mode**: never drop raw buffers before CIC. If raw CIC line rate
  exceeds the maximum rate, use a credit-based fractional keep ratio after each
  processed FFT line:
  `keep_ratio = min(rate_limit_lps * 0.85, raw_cic_lps) / raw_cic_lps`.
  Accumulate `keep_ratio` per completed line; publish when credit reaches
  `>= 1.0`. The publish-clock guard also uses the same `0.85` factor.

Level scaling:

```text
fft_magnitude_scale = 1 / sum(window)
cic_dc_scale = 1 / decim^CIC_STAGES
cic_runtime = bounded moving sums with CIC_STAGES * decim complex delay entries
cic_frequency_weight = min(1 / max(abs(sin(decim*pi*f/fs) / (decim*sin(pi*f/fs)))^CIC_STAGES, 0.05), 8)
```

Do not auto-start a saved scan when the backend is idle. The page may restore saved fields and view state, but actual scanning should require Start unless the backend was already scanning before the browser attached.

## Tests

Before handing changes back:

```sh
make check
tools/http_smoke_test.sh
tools/cic_stability_check.py
tools/cic_continuity_check.py
tools/cic_synthetic_signal_check.py
tools/headless_tester.py
tools/ui_browser_test.py
tools/phase3_browser_stress.py
tools/zoom_sweep.py --use-existing --freq-start-mhz 70 --freq-end-mhz 1000 --min-rate-lps 10 --rate-limit-lps 20 --out zoom-rate-matrix.md
tools/browser_zoom_matrix.py --output browser-zoom-matrix.json --freq-start-mhz 70 --freq-end-mhz 1000 --min-rate-lps 10 --rate-limit-lps 20 --settle-seconds 4
tools/fm_screenshot.py --output images/fm-broadcast-waterfall.png --wait-seconds 60
tools/frontend_random_validation.py --output frontend-random-validation.json
```

For changes touching CIC decimation, also run `tools/cic_stability_check.py`,
`tools/cic_continuity_check.py`, `tools/cic_synthetic_signal_check.py`,
`tools/min_rate_overlap_check.py`, `tools/cached_preview_check.py`, and, when
Pluto hardware is available, live `FFT=65536 x2` plus deeper `x64` runs for
several minutes. Confirm that reception remains stable and that backend logs
show the expected `overlap`, `CIC samples:` with `accounting errors 0`, no
`CIC sample-order errors`, no repeated read retries, watchdog cancels, hard
reconnects, or excessive `CIC queue waited ...` backpressure. The synthetic
test must pass clean x2, x64, and x256 tones with min-rate overlap enabled and
reject periodic skip/duplicate controls.

The browser tester expects Firefox, Selenium, and geckodriver. Override the Firefox path with:

```sh
FIREFOX_BIN=/path/to/firefox tools/ui_browser_test.py
```

For a deeper frontend pass, run the scanner with trace capture:

```sh
PLUTO_URI=ip:pluto.local stdbuf -oL -eL ./pluto-scanner 2>&1 | tee frontend-validation-trace.log
```

Then run the browser validations in another terminal. Check the trace for:

- `Closing SDR device after I/O error`
- `Device reconnected`
- repeated hard `pluto_sdr_read_async failed` without recovery

For FFT/CIC planning, `tools/frontend_random_validation.py` verifies:

- Scan/hop mode stays in `mode=scan`, `steps>=2`, `decim_factor=1`, and uses adaptive `effective_fft_size` values from `1024` to `8192`; published bins must equal display bins.
- Narrow single-frequency mode switches to `mode=single`, chooses the active hardware sample rate/RF bandwidth profile from the formulas above, keeps RF bandwidth less than sample rate, uses a power-of-two CIC decimation factor when required, and reports raw line rate from the empirical Pluto host-throughput model rather than the RF sampling frequency.
