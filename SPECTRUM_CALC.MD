# Spectrum Coefficient Calculation

Reference table display width: `1920` pixels. The implementation uses the
runtime `display_bins` value supplied by the frontend; `1920` is only the
worked example width for the table below.

Default frequency range: `70..1000 MHz`, full span `930 MHz`.

Waterfall-rate budget used for this layout: minimum `10` lines/s, maximum
`20` lines/s.

## Pluto RF Constraints

Official Pluto documentation lists the ADALM-Pluto as an AD9363 radio with
variable output data rates up to `61.44 MSPS` and tunable channel bandwidth
from `200 kHz` to `20 MHz`.

The AD9363 data sheet also describes a direct-conversion receiver and channel
bandwidths below `200 kHz` through `20 MHz`. The libad9361 automatic filter
helper documents a default filter design with `Fpass = sample_rate / 3` and
`Fstop = Fpass * 1.25`.

This scanner therefore uses a deliberately conservative displayed passband for
single-frequency views:

```text
displayed_rf_bandwidth_hz <= sample_rate_hz / 2
displayed_rf_bandwidth_hz <= 20e6
rf_bandwidth_hz < sample_rate_hz
```

Complex IQ sampling can represent a wider mathematical baseband than this.
The half-rate displayed passband is an engineering limit for flatter waterfall
edges and fewer AD9363/filter-edge artifacts, not a hardware register maximum.

## Air Frequency and Converter Preconditions

All spans and centers in this document are air/signal frequencies. The Pluto
receiver range is applied only after converter math:

```text
rx_hz = air_hz - converter_hz          when converter_hz >= 0
rx_hz = abs(-converter_hz - air_hz)    when converter_hz < 0
```

For a non-negative converter, the valid air interval is:

```text
[converter_hz + 70e6, converter_hz + 6000e6]
```

For a negative converter (`conv_hz = -converter_hz`), the valid air intervals
are:

```text
[max(0, conv_hz - 6000e6), conv_hz - 70e6]
[conv_hz + 70e6, conv_hz + 6000e6]
```

The configured air band must be wholly inside one valid interval before the
planner runs. Invalid converted bands are rejected; the planner must not clamp
or translate the user's configured air band to a nearby valid receiver range.
The frontend must also avoid using `6000 MHz` as a maximum air-frequency input
limit, because converter-mapped air frequencies can be above `6 GHz`.

Sources consulted:

- ADALM-Pluto detailed specifications:
  https://wiki.analog.com/university/tools/pluto/devs/specs
- AD9363 data sheet:
  https://www.analog.com/media/en/technical-documentation/data-sheets/ad9363.pdf
- libad9361 automatic filter notes:
  https://github.com/analogdevicesinc/libad9361-iio/blob/main/ad9361.h

## RF Profiles

Scan/hop mode keeps the fast transport profile because each hop captures a
short buffer instead of an uninterrupted long USB/network stream:

```text
sample_rate = 61.44 MSPS
rf_bandwidth = 20 MHz
passband_usage = 0.85
hop_step = 17 MHz
```

Single-frequency mode uses conservative displayed passband profiles:

```text
(4 MSPS, 2 MHz),
(8 MSPS, 4 MHz),
(16 MSPS, 8 MHz),
(20 MSPS, 10 MHz),
(30.72 MSPS, 15 MHz),
(61.44 MSPS, 20 MHz)
```

## Desired Zoom Table

`Output bins` is always exactly `display_bins`: one processed output value per
screen pixel. `Raw source bins` is the number of FFT/CIC bins in the source
line before the exact display-bin reducer. `visible raw bins/px` is the raw
FFT/CIC-bin density across the visible slice; it should stay at or above `1.0`
where hardware limits allow it.

The SSE transport sends those `display_bins` values as packed `uint8` bytes
base64 encoded in `encoding:"u8b64"` / `db`. This reduces wire text overhead
but does not change any FFT, CIC, weighting, or line-rate formula below.

`2nd IF kHz` is the effective LO/source-center offset from the visible center.
It moves the zero-IF/DC artifact away from the middle of the screen. When the
source span is wide enough, zero IF is outside the visible span by the listed
`Zero guard kHz`. At the deepest CIC zooms, the source span is too narrow for a
full `50 kHz` guard, so the offset becomes the largest possible value while
still covering the visible span.

`FFT weight` is the Hann coherent-gain correction, `1 / sum(window)`.
`CIC DC scale` is `1 / decim^3`. `CIC edge weight` is the maximum CIC droop
compensation expected at the visible edge for that row.

| Zoom | Span MHz | Mode | SR MSPS | RF/display BW MHz | PB | Hops | 2nd IF kHz | Zero guard kHz | Decim | FFT | Raw source bins | Output bins | visible raw bins/px | FFT weight | CIC DC scale | CIC edge weight | line ms | first/raw ms |
| ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 930 | scan | 61.44 | 20.00 | 0.85 | 55 | 0.0 | 0.0 | 1 | 1024 | 15482 | 1920 | 8.06 | 0.0019550342 | 1 | 1.000 | 187 | 187 |
| 2 | 465 | scan | 61.44 | 20.00 | 0.85 | 28 | 0.0 | 0.0 | 1 | 1024 | 7741 | 1920 | 4.03 | 0.0019550342 | 1 | 1.000 | 95 | 95 |
| 5 | 186 | scan | 61.44 | 20.00 | 0.85 | 11 | 0.0 | 0.0 | 1 | 1024 | 3097 | 1920 | 1.61 | 0.0019550342 | 1 | 1.000 | 59 | 37 |
| 10 | 93 | scan | 61.44 | 20.00 | 0.85 | 6 | 0.0 | 0.0 | 1 | 2048 | 3102 | 1920 | 1.62 | 0.00097703957 | 1 | 1.000 | 59 | 29 |
| 20 | 46.5 | scan | 61.44 | 20.00 | 0.85 | 3 | 0.0 | 0.0 | 1 | 4096 | 3099 | 1920 | 1.61 | 0.00048840049 | 1 | 1.000 | 59 | 18 |
| 50 | 18.6 | scan | 61.44 | 20.00 | 0.85 | 2 | 0.0 | 0.0 | 1 | 8192 | 2480 | 1920 | 1.29 | 0.00024417043 | 1 | 1.000 | 59 | 18 |
| 100 | 9.3 | single | 20.00 | 10.00 | 0.50 | 1 | 350.0 | 50.0 | 1 | 8192 | 4096 | 1920 | 1.98 | 0.00024417043 | 1 | 1.000 | 59 | 4 |
| 200 | 4.65 | single | 16.00 | 8.00 | 0.50 | 1 | 1675.0 | 50.0 | 1 | 8192 | 4096 | 1920 | 1.24 | 0.00024417043 | 1 | 1.000 | 59 | 4 |
| 500 | 1.86 | single | 4.00 | 2.00 | 0.50 | 1 | 70.0 | 50.0 | 1 | 8192 | 4096 | 1920 | 1.98 | 0.00024417043 | 1 | 1.000 | 59 | 4 |
| 1000 | 0.93 | single | 4.00 | 2.00 | 0.50 | 1 | 515.0 | 50.0 | 1 | 16384 | 8192 | 1920 | 1.98 | 0.00012207776 | 1 | 1.000 | 59 | 8 |
| 2000 | 0.465 | single | 4.00 | 2.00 | 0.50 | 1 | 282.5 | 50.0 | 1 | 32768 | 16384 | 1920 | 1.98 | 0.000061037019 | 1 | 1.000 | 59 | 16 |
| 5000 | 0.186 | single | 4.00 | 2.00 | 0.50 | 1 | 143.0 | 50.0 | 1 | 65536 | 32768 | 1920 | 1.59 | 0.000030518044 | 1 | 1.000 | 59 | 31 |
| 10000 | 0.093 | single | 4.00 | 2.00 | 0.50 | 1 | 96.5 | 50.0 | 2 | 65536 | 65536 | 1920 | 1.59 | 0.000030518044 | 0.125 | 1.019 | 71 | 71 |
| 20000 | 0.0465 | single | 4.00 | 2.00 | 0.50 | 1 | 73.2 | 50.0 | 4 | 65536 | 65536 | 1920 | 1.59 | 0.000030518044 | 0.015625 | 1.044 | 100 | 142 |
| 50000 | 0.0186 | single | 4.00 | 2.00 | 0.50 | 1 | 59.3 | 50.0 | 8 | 65536 | 65536 | 1920 | 1.27 | 0.000030518044 | 0.001953125 | 1.096 | 100 | 283 |
| 100000 | 0.0093 | single | 4.00 | 2.00 | 0.50 | 1 | 54.6 | 50.0 | 16 | 65536 | 65536 | 1920 | 1.27 | 0.000030518044 | 0.00024414062 | 1.326 | 100 | 567 |
| 200000 | 0.00465 | single | 4.00 | 2.00 | 0.50 | 1 | 52.3 | 50.0 | 32 | 65536 | 65536 | 1920 | 1.27 | 0.000030518044 | 0.000030517578 | 2.745 | 100 | 1134 |
| 500000 | 0.00186 | single | 4.00 | 2.00 | 0.50 | 1 | 30.3 | 50.0 | 64 | 65536 | 65536 | 1920 | 1.02 | 0.000030518044 | 0.0000038146973 | 3.875 | 100 | 2267 |
| 1000000 | 0.00093 | single | 4.00 | 2.00 | 0.50 | 1 | 15.2 | 50.0 | 128 | 65536 | 65536 | 1920 | 1.02 | 0.000030518044 | 0.00000047683716 | 3.875 | 100 | 4534 |

## Goals

- Use exactly `1` processed output bin per screen pixel across the visible
  span.
- Keep enough raw FFT/CIC bins before reduction so each output bin is derived
  from real spectrum data.
- Keep single-frequency displayed passband no wider than half the active
  sample rate, with the Pluto `20 MHz` RF bandwidth cap.
- Keep RF bandwidth strictly lower than sample rate in every hardware profile.
- Keep scan/hop mode fast at low zoom by using short FFTs, increasing scan FFT
  only where the raw-bin product would otherwise drop below the display width.
- In single-frequency mode, reduce Pluto hardware sample rate/RF bandwidth
  before using CIC.
- Use CIC only when the smallest strict hardware profile (`4 MSPS / 2 MHz`) and
  `FFT=65536` still need more raw-bin product.
- Implement the CIC response with bounded moving sums, not unbounded runtime
  integrators, so long-running captures do not lose floating-point precision.
- Preserve raw-buffer continuity before CIC. Software queue backpressure may
  wait, but it must not drop raw buffers that feed the stateful decimator.
- Use four queued IIO kernel blocks for continuous single-frequency capture.
  The one-block scan/hop profile does not provide enough upstream buffering for
  a stateful decimator while userspace extracts, converts, and copies a refill.
- Place zero IF outside the visible span by `50 kHz` when the source span has
  enough room; otherwise use the largest offset that still covers the visible
  range.
- Normalize FFT output so a coherent tone has the same displayed level across
  FFT size, CIC decimation, and CIC passband position.
- Keep SSE rows at exactly `display_bins` values. Raw FFT bins are reduced to
  pixels by peak-per-pixel mapping; raw counts are reported as debug metadata.
- Preserve persistent waterfall history across zoom and frequency changes.
  The principle is known in SDR UIs, but the implementation should improve it
  by keeping historical rows useful instead of clearing the display whenever
  the user changes view.
- Hide avoidable resolution-change latency. Very fine frequency resolution
  needs FFTs built from many seconds of samples, so compatible cached samples
  should provide the first preview rows while live acquisition fills the new
  FFT/CIC window.

## Formulas

Definitions:

```text
display_bins    = runtime frontend display width (1920 in this table)
full_span_hz    = 1000e6 - 70e6
visible_span_hz = full_span_hz / zoom
scan_sr_hz      = 61.44e6
scan_rf_bw_hz   = 20e6
scan_pb         = 0.85
scan_step_hz    = scan_rf_bw_hz * scan_pb
scan_fft_min    = 1024
scan_fft_max    = 8192
single_fft_min  = 2048
fft_max         = 65536
decim_max       = 4096
single_decim_async_min_samples = 8192
single_decim_async_max_samples = 262144
zero_if_guard_hz = 50000
```

Scan/hop mode:

```text
hops = ceil(visible_span_hz / scan_step_hz)
mode = scan when hops > 1

required_scan_fft =
  display_bins * scan_sr_hz / visible_span_hz

scan_fft =
  clamp_pow2(required_scan_fft, 1024, 8192)

raw_source_bins =
  sum(round(hop_width_hz / scan_sr_hz * scan_fft))

output_bins =
  display_bins

visible_raw_bins_per_pixel =
  raw_source_bins / display_bins
```

Estimated scan timing:

```text
hop_ms(1024) = 3.4
hop_ms(2048) = 4.8
hop_ms(4096) = 6.0
hop_ms(8192) = 8.9

raw_lps = 1000 / (hop_ms(scan_fft) * hops)
draw_lps =
  raw_lps                         when raw_lps <= max_rate_lps
  max_rate_lps * 0.85             when raw_lps > max_rate_lps
line_interval_s = 1 / draw_lps
```

Single-frequency mode:

```text
single_profiles =
  (4e6, 2e6),
  (8e6, 4e6),
  (16e6, 8e6),
  (20e6, 10e6),
  (30.72e6, 15e6),
  (61.44e6, 20e6)

mode = single when visible_span_hz <= scan_step_hz

required_profile_span_hz =
  visible_span_hz + 2 * zero_if_guard_hz

hardware profile =
  smallest profile whose min(sample_rate, rf_bandwidth)
  covers required_profile_span_hz

required_product =
  ceil(display_bins * hardware_sample_rate_hz / visible_span_hz)

decim =
  clamp_pow2(ceil(required_product / fft_max), 1, decim_max)

while decim > 1 and hardware_sample_rate_hz / decim < visible_span_hz * 1.05:
  decim = decim / 2

fft =
  clamp_pow2(ceil(required_product / decim), single_fft_min, fft_max)

fft_sample_rate_hz =
  hardware_sample_rate_hz / decim

source_span_hz =
  min(hardware_rf_bandwidth_hz, fft_sample_rate_hz)

raw_source_bins =
  round(source_span_hz / fft_sample_rate_hz * fft)

visible_raw_bins =
  raw_source_bins * visible_span_hz / source_span_hz

output_bins =
  display_bins

visible_raw_bins_per_pixel =
  visible_raw_bins / display_bins
```

Effective second-IF placement:

```text
visible_center_hz =
  (visible_start_hz + visible_end_hz) / 2

outside_zero_if_shift_hz =
  visible_span_hz / 2 + zero_if_guard_hz

max_covering_shift_hz =
  max(0, (source_span_hz - visible_span_hz) / 2)

second_if_hz =
  min(outside_zero_if_shift_hz, max_covering_shift_hz)

source_center_hz =
  visible_center_hz +/- second_if_hz

Choose the sign that keeps source_start/source_end covering the visible span
and keeps the converted Pluto receiver LO inside the valid 70..6000 MHz range.
```

Estimated single timing:

```text
raw_stream_sps =
  min(hardware_sample_rate_hz, 2.1e6)

cic_stream_sps =
  min(hardware_sample_rate_hz, 1.85e6)

first_line_s =
  (fft + CIC_STAGES) * decim / cic_stream_sps   when decim > 1
  fft / raw_stream_sps                          when decim == 1

nonoverlap_cic_lps =
  cic_stream_sps / (fft * decim)

target_lps =
  min(min_rate_lps, max_rate_lps)  when decim > 1 and min_rate_lps > 0

required_overlap =
  ceil(target_lps / nonoverlap_cic_lps)

overlap_factor =
  min(next_power_of_two(max(1, required_overlap)), fft)

decim_hop =
  fft / overlap_factor             when decim > 1 and min_rate_lps > 0
  fft                              otherwise

line_samples =
  decim_hop * decim                  when decim > 1
  fft                                when decim == 1

async_samples =
  min(line_samples, single_decim_async_max_samples)  when decim > 1
  fft                                                when decim == 1

steady_lps =
  cic_stream_sps / (decim_hop * decim)  when decim > 1
  raw_stream_sps / fft                  when decim == 1

draw_lps =
  steady_lps                      when steady_lps <= max_rate_lps
  max_rate_lps * 0.85             when steady_lps > max_rate_lps

line_interval_s =
  1 / draw_lps
```

The throttle point depends on the mode: scan and non-CIC single mode may drop
whole pre-FFT buffers or cycles, while CIC single mode must preserve every raw
buffer until the CIC-decimated FFT line is complete and may only drop after
that processed line exists.

Level normalization:

```text
window_scale =
  1 / sum(hann_window)
  approximately 2 / (fft_size - 1)

cic_dc_scale =
  1 / decim^CIC_STAGES

cic_frequency_gain(f) =
  abs(sin(decim * pi * f / raw_sample_rate) /
      (decim * sin(pi * f / raw_sample_rate))) ^ CIC_STAGES

cic_frequency_weight(f) =
  min(1 / max(cic_frequency_gain(f), 0.05), 8)

fft_output_level =
  abs(FFT(windowed_samples)) * window_scale * cic_frequency_weight(f)

cic_decimated_sample =
  cic_output_sample * cic_dc_scale
```

Runtime CIC realization:

```text
For each raw complex sample x[n], with D = decim:

stage0[n] = sum_{k=0..D-1} x[n-k]
stage1[n] = sum_{k=0..D-1} stage0[n-k]
stage2[n] = sum_{k=0..D-1} stage1[n-k]

Every D raw samples:
  cic_output_sample = stage2[n] / D^CIC_STAGES
```

The implementation keeps one ring buffer of `D` complex values per CIC stage,
for `CIC_STAGES * D` complex delay entries. This is equivalent to the
documented 3-stage CIC response with differential delay 1, but it avoids the
unbounded integrator growth that would otherwise accumulate floating-point
error over long runs. The raw buffers feeding this state must remain
contiguous; dropping queued buffers before CIC creates transients that can look
like widened signals in the waterfall.

Queue block ordinals and logical source ranges validate the software path from
callback acceptance through CIC processing. They cannot identify samples
already omitted by hardware or transport because Pluto RX data has no sample
timestamp. Continuous IIO queue depth, explicit recovery flags, synthetic
spectral controls, and long live runs cover that boundary.

Minimum line-rate requests do not change FFT size, CIC decimation, hardware
sample rate, RF bandwidth, source span, or visible raw-bin density. In CIC mode
they reduce `decim_hop` with a power-of-two integer `overlap_factor`, so raw
samples still enter the bounded CIC once while adjacent FFT windows
intentionally reuse already-decimated samples.

For ordinary decimated single-frequency plans, the Pluto async refill is sized
to `line_samples`, so overlap-active plans request one hop worth of new raw
samples per callback when possible. With min rate disabled, the common
`FFT=65536 x2` case uses `async_samples = 131072` and one hardware refill
supplies one displayed FFT line. Extremely large decimated line advances are capped by
`single_decim_async_max_samples`; logs must expose capped cases as
`async < line samples` for diagnosis.

Historical preview rows reduce the first-line wait after compatible high-zoom
view changes. Very fine frequency resolution may need many seconds of samples
for one FFT; without a preview path, switching resolution traditionally leaves
the user waiting for the fresh decimated stream to fill the new FFT/CIC window.
When compatible cached samples exist, the backend may snapshot recent raw
samples, apply the required center-frequency phase rotation, and replay them
through the same CIC/Hann/FFT path. The required raw history for preview rows
is:

```text
preview_raw_samples =
  (CIC_STAGES + fft + (preview_lines - 1) * decim_hop) * decim
```

Preview rows are valid historical measurements for display responsiveness, but
they are marked `preview=1` and are never joined to post-restart live samples.
Live CIC state is reset after preview processing, and the first live row is
marked `preview=0`.

`tools/cic_stability_check.py` validates the bounded-state realization with a
tone plus DC offset for decimations `2`, `4`, `8`, `64`, and `256`. It is a numeric
regression check for state growth and steady amplitude, not a substitute for a
live long-run `FFT=65536 x2` hardware pass or multi-tone/noise-floor testing.

`tools/cic_synthetic_signal_check.py` passes a deterministic complex tone
through the C queue/CIC/Hann/FFT/SSE path at x2, x64, and x256. Clean cases must
have a Hann main lobe no wider than three bins and distant leakage below
`-70 dBc`; periodic skip and duplicate controls must violate the spectral gate
and produce source-range errors.

For the table's `CIC edge weight`, use:

```text
edge_frequency_hz =
  min(source_span_hz / 2, second_if_hz + visible_span_hz / 2)

cic_edge_weight =
  cic_frequency_weight(edge_frequency_hz)
```
