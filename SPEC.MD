# Pluto SDR Scanner — Technical Specification

## 1. Purpose

A browser-based real-time spectrum analyser and waterfall display for the
ADALM-Pluto software-defined radio. The application covers the extended
receiver range advertised by the connected Pluto, displays the full configured
band at once, and supports continuous zoom up to **1,000,000×**
(hertz-per-pixel detail).
It does not require custom Pluto firmware: the original Analog Devices stock
firmware and DATV firmware variants are supported when the standard libiio
devices and AD936x attributes are present.

Design goals (from `README.md`):

1. AI-first development workflow.
2. A web interface that minimizes traffic (waterfall rows are exactly one
   byte per visible CSS pixel; only planning metadata rides alongside).
3. One-million-times zoom: from gigahertz-wide full-screen spans down to
   hertz-per-pixel detail.
4. Seamless merging of scan/hop mode and single-frequency reception, hidden
   from the user.
5. Waterfall speed expressed as a line-rate range (min/max lines/s) rather
   than being tied directly to FFT size.
6. Persistent waterfall history — zoom/pan reuses recorded rows instead of
   clearing the screen. The principle is known in SDR UIs, but this program
   treats it as an area to improve so history remains useful across large zoom
   and frequency changes.
7. Low-latency, smooth resolution changes — compatible high-zoom view changes
   keep capture running. The new integer-CIC/Hann/FFT generation starts from
   indexed raw history and joins the same capture timeline at an exact sample
   boundary. FFT size is the smallest power of two that covers the visible CSS
   pixels; CIC supplies deeper resolution.
8. Exact frequency tuning — deterministic compensation for PLL and DDS-style
   rounding errors keeps received signals plotted at their true frequencies.

---

## 2. Architecture Overview

```text
+-------------+      IIO buffers       +--------------------------------+
| ADALM-Pluto | ---------------------> | C backend: main.c              |
+-------------+                        |                                |
                                       | scan/capture thread            |
+----------------------+               |   -> indexed Q15 raw history   |
| Frontend: index.html | -- JSON API ->|   -> worker queue              |
| controls and dialogs |               |   -> NCO / CIC / Hann / FFT    |
| spectrum/waterfall   | <- SSE rows --|   -> CSS-pixel row reducer     |
| rulers and markers   |               |   -> SSE broadcaster           |
+----------------------+               +--------------------------------+
```

The backend is a **single C99 source file** (`main.c`) compiled with `gcc`,
`libiio`, `libm`, and `pthreads`. The frontend is a **single HTML file** with
no bundled dependencies. No external HTTP, UI, or JS framework is used.

---

## 3. Backend: `main.c`

### 3.1 Responsibilities

- **HTTP server** on `DEFAULT_BIND_ADDRESS:DEFAULT_HTTP_PORT`
  (`127.0.0.1:8080`). A bare `socket()/bind()/listen()/select()` loop (no
  library), using `getaddrinfo()` so `--bind` may select IPv4, IPv6,
  localhost, or all trusted LAN interfaces. Bounded request buffer
  `MAX_REQUEST = 65536` bytes, 5 s receive timeout per client.
- **Pluto SDR control** through `libiio`:
  - Opens an IIO context, locates `ad9361-phy` and `cf-ad9361-lpc`.
  - Writes RX LO (`altvoltage0` frequency), `sampling_frequency`,
    `rf_bandwidth`, `gain_control_mode`, `hardwaregain`, `rf_port_select`.
  - Creates IIO buffers, refills them, decodes I/Q.
  - Retunes and discards stale buffers during scan hops.
  - Uses only the standard IIO/AD936x control surface, so stock Pluto firmware
    and DATV firmware variants do not need to be reflashed for this program.
  - Consumes one complex RX channel (`cf-ad9361-lpc` I/Q) and currently fixes
    its RF port to `A_BALANCED`. Additional receiver channels/port routing are
    deferred.
- **FFT engine** — radix-2 complex-to-complex in-place transform, Hann window.
- **CIC decimator** — 3-stage CIC response for narrow-span views, implemented
  as bounded moving sums to avoid unbounded numeric state.
- **Dual-mode planner** — automatically selects scan/hop vs single-frequency.
- **SSE broadcast** — publishes waterfall rows as `event: line` to all clients.
- **Persistent config** — `pluto-scanner.conf` saved/restored on every change,
  including the explicit `resume_scan` receive intent.
- **Watchdog & reconnect** — detects Pluto I/O stalls and reopens the device.

### 3.2 Threading model

| Thread | Created by | Purpose |
|---|---|---|
| Main / HTTP | `main()` | `select()` loop, accepts clients, dispatches `handle_request`. |
| Scan thread | `start_scan()` | Drives Pluto acquisition; test builds may synthesize random data or a continuous complex tone. |
| Worker thread | `scan_thread_func` | Consumes captured buffers from a bounded queue (`PROCESS_QUEUE_LEN = 8`) and produces FFT/CIC lines. |
| Watchdog thread | `scan_thread_func` | Detects acquisition stalls (8 s startup, 5 s steady) and triggers cancel. |

Buffers flow: Pluto refill → `scan_callback` → `scan_queue_push`
(mutex/condition ring) → worker thread `process_scan_buffer` →
`publish_scan_line` → `sse_broadcast`. Non-CIC paths may drop a buffer when
the queue is full. Single-frequency CIC mode waits for queue space instead,
because raw-buffer gaps corrupt the stateful decimator and widen signals in
the waterfall.

### 3.3 Key constants (`main.c` constants block)

| Constant | Value | Purpose |
|---|---|---|
| `DEFAULT_HTTP_PORT` | 8080 | Default HTTP port; override with `--port` |
| `DEFAULT_BIND_ADDRESS` | `127.0.0.1` | Default local-only bind address; override with `--bind` |
| `MAX_CLIENTS` | 64 | Concurrent HTTP clients |
| `MAX_SSE_CLIENTS` | 16 | Concurrent waterfall subscribers |
| `MAX_REQUEST` | 65536 | Max request size (bytes) |
| `SCANNER_SAMPLE_RATE_HZ` | 61.44 MHz | Pluto default rate |
| `DEFAULT_RF_BANDWIDTH_HZ` | 20 MHz | RF bandwidth |
| `DEFAULT_BW_RATIO` | 0.85 | Passband usage per hop |
| `RF_RECEIVER_FALLBACK_MIN_HZ` | 46,875,001 Hz | Default receiver low end until Pluto reports `frequency_available` |
| `RF_RECEIVER_FALLBACK_MAX_HZ` | 6000 MHz | Default receiver high end until Pluto reports `frequency_available` |
| `SCAN_BUF_LEN` | 1024 | Short hop buffer (complex samples) |
| `PLUTO_SCAN_KERNEL_BUFFERS` | 1 | Low-latency libiio queue depth for retuned scan/hop captures |
| `PLUTO_CONTINUOUS_KERNEL_BUFFERS` | 4 | Queued libiio blocks for uninterrupted single-frequency capture |
| `PLUTO_DISCARD_COUNT` | 2 | Stale buffers dropped per hop |
| `PLUTO_TUNE_RETRY_COUNT` | 10 | LO-set retries before give-up |
| `CIC_STAGES` | 3 | CIC response stages implemented as bounded moving sums |
| `FFT_SIZE_MIN` / `MAX` | 1024 / 65536 | FFT bounds (powers of two) |
| `SCAN_FFT_SIZE_MAX` | 8192 | Scan-mode FFT cap |
| `SINGLE_RAW_FFT_MIN` | 2048 | Single-mode FFT floor |
| `SINGLE_DECIM_MAX` | 65536 | Largest integer CIC factor |
| `SINGLE_DECIM_ASYNC_MIN_LEN` | 8192 | Smallest raw line advance targeted for CIC refills |
| `SINGLE_DECIM_ASYNC_MAX_LEN` | 262144 | Largest CIC async refill before capping queue memory |
| `CACHED_PREVIEW_MAX_SAMPLES` | 16,777,984 | Restart-preview raw-sample bound |
| `CACHED_HISTORY_MAX_SAMPLES` | 33,555,968 | Q15 complex samples retained for hot handoff |
| `SINGLE_ZERO_IF_GUARD_HZ` | 50000 | Zero-IF clearance |
| `PLUTO_REFERENCE_INPUT_HZ` | 40 MHz | Physical Pluto reference input used by the coordinate model |
| `PLUTO_RFPLL_REFERENCE_HZ` | 80 MHz | AD936x driver RFPLL parent after the 40 MHz reference doubler |
| `PLUTO_RFPLL_FRACTIONAL_MODULUS` | 8388593 | AD936x fractional-N model modulus |
| `PLUTO_EST_SINGLE_STREAM_SPS` | 2.1e6 | Empirical host throughput (raw) |
| `PLUTO_EST_CIC_STREAM_SPS` | 1.85e6 | CIC-path throughput |
| `PLUTO_RATE_LIMIT_GUARD` | 0.85 | Max-rate throttle guard |
| `PLUTO_DEFAULT_GAIN_DB` | 20 | Default hardware gain |
| `PLUTO_GAIN_MIN/MAX_DB` | −3 / 71 | Gain limits |
| `DB_FLOOR` / `DB_CEIL` | −100 / −20 | Display dB window |
| `FRONTEND_IDLE_STOP_MS` | 20000 | Stop scan after 20 s idle frontend |

### 3.3.1 Command line

The backend is reachable only from the local machine by default:

```sh
./pluto-scanner
```

This binds `127.0.0.1:8080` and opens Pluto through the saved/default
`libiio` URI. Runtime overrides are:

| Option | Meaning |
|---|---|
| `--uri <uri-or-host>` / `--uri=<uri-or-host>` | Override the Pluto IIO URI. Bare hosts such as `192.168.2.1` are normalized to `ip:192.168.2.1`. |
| `--port <1..65535>` / `--port=<1..65535>` | Override the incoming HTTP port. |
| `--bind <address>` / `--bind=<address>` | Override the HTTP bind address. Use `127.0.0.1` for local-only, `0.0.0.0` or `::` for trusted LAN access. |
| `--listen <address>` / `--listen=<address>` | Alias for `--bind`. |

The program does not implement authentication, TLS, per-user sessions, or
multi-user arbitration. Binding to a non-loopback address should therefore be
treated as trusted-network-only operation. For wildcard LAN binds, the startup
banner prints `localhost` plus reachable `http://...` URLs for active
non-loopback interface addresses where they can be enumerated. For a concrete
non-loopback bind, it prints the bound address.

### 3.4 Operational modes

The mode decision (`planned_run_mode`) is:

```
required_points = ceil(visible_span / (rf_bandwidth * bw_ratio))
mode = (required_points > 1) ? SCAN : SINGLE
```

> **Scan/hop mode** (wide spans) — retunes Pluto across the visible span in
> `required_points` steps. Each step: set RX LO → discard 2 stale buffers →
> refill 1 useful buffer → FFT → average per-step bins. The per-step bins are
> stitched into one raw line, then reduced to `display_bins` by peak-per-pixel
> aggregation.
>
> **Single-frequency mode** (narrow spans) — tunes Pluto to one center and
> streams continuously. Selects a conservative hardware profile from
> `(4/2), (8/4), (16/8), (20/10), (30.72/15), (61.44/20)` MSPS/MHz, then
> starts with the smallest power-of-two FFT covering the visible CSS pixels and
> applies the smallest feasible integer CIC factor needed for resolution.
> Uses a 3-stage CIC response with DC-gain normalization (`decim^−3`) and
> per-bin inverse droop compensation clamped to `[0.05, 8.0]×`. The runtime
> implementation uses three bounded moving sums of length `decim`, rather than
> unbounded integrators, so state remains stable during long captures. Minimum
> line-rate requests keep the same hardware profile, FFT size, and CIC
> decimation, then use integer overlap on the decimated stream by reducing
> `decim_hop`. Raw samples must still enter the CIC exactly once and in order.
> A compatible view change leaves IIO capture and the physical LO running. The
> worker initializes the new generation from indexed Q15 raw history ending at
> one handoff sample, publishes that row as `preview=1`, consumes all samples
> captured after the handoff, and resumes queued `preview=0` rows on the same
> continuity epoch. The old and new DSP centers must each remain inside the
> other's decimated source span; larger center changes use the bounded
> restart-preview path so out-of-source old-view carriers are not folded into
> the new narrow source by CIC aliasing. Incompatible changes also restart.
> The physical LO keeps zero IF at least `50 kHz` outside the visible span. A
> phase-continuous pre-CIC NCO moves the uninterrupted physical stream to the
> visible DSP center before decimation.

All coefficient math (zoom table, profile selection, FFT/CIC planning, line
rates, level normalization) is fully specified in **`SPECTRUM_CALC.MD`**.

### 3.4.1 Air-frequency and converter contract

All frontend controls, API request fields, persisted config fields, SSE row
frequencies, and planner spans are **air/signal frequencies**. The backend
reads the AD936x RX LO `frequency_available` attribute once after opening the
Pluto and exposes the active receiver limits as `receiver_min_hz` and
`receiver_max_hz`. If that attribute is missing or malformed, the fallback is
`46,875,001 Hz..6,000,000,000 Hz`. The Pluto receiver range is enforced only
after converter math:

```text
rx_hz = air_hz - converter_hz          when converter_hz >= 0
rx_hz = abs(-converter_hz - air_hz)    when converter_hz < 0
```

Valid configured air-frequency intervals are therefore:

```text
converter_hz >= 0:
  [converter_hz + receiver_min_hz, converter_hz + receiver_max_hz]

converter_hz < 0, conv_hz = -converter_hz:
  [max(0, conv_hz - receiver_max_hz), conv_hz - receiver_min_hz]
  [conv_hz + receiver_min_hz, conv_hz + receiver_max_hz]
```

The frontend may normalize typed Start/End fields into the active valid
air-frequency interval for the current converter, but it must not clamp
air/signal inputs to raw `6000 MHz` without converter math; converter-mapped
air frequencies may legitimately be above `6 GHz`. `/api/start` must reject a
configured band that is not wholly inside one valid converted receiver interval
with HTTP `400`, and it must do so before mutating `g_freq_start`/`g_freq_end`.

### 3.4.2 RFPLL coordinate correction

`fq_err_correction = 1` is a local configuration flag enabled by default. In
single-frequency mode, the backend applies the exact integer-hertz `llround()`
used for the IIO RX-LO write, then the AD936x driver's even-hertz clock bridge
(`freq >> 1`, followed by `freq << 1`), and finally models fractional-N RFPLL
quantization from the stock driver's doubled 80 MHz RFPLL parent for a 40 MHz
Pluto reference input, and the power-of-two divider that places the RF VCO in
the driver range above 6 GHz and up to 12 GHz. The fractional tuning word is
computed with integer arithmetic matching the ADI driver:
`fract = floor((remainder * 8388593 + parent / 2) / parent)`. It converts the signed
actual-minus-requested receive-LO error through the active converter branch and
shifts the *source-bin coordinate* by that per-tune amount. The user-visible air range and IIO
`altvoltage0/frequency` request do not change. No fixed local calibration
constant is supported. `GET /api/status` exposes `fq_err_correction`,
`fq_err_model_hz`, and `fq_err_effective_hz`; SSE row metadata exposes the
applied `fq_err_correction_hz` and interpolated `raw_peak_hz` for live
verification. The RFPLL model is a diagnostic estimate; validate hardware
alignment against a known reference before treating a remaining offset as
software error.

Hertz coordinates in API responses and SSE row metadata are serialized with
fractional precision. Whole-Hz JSON formatting is not allowed for
`visible_*_hz`, `scan_*_hz`, `f0`, `f1`, `second_if_hz`, `step_hz`, or other
coordinate fields because one-million zoom views need sub-Hz placement.

### 3.5 Acquisition loop details

For scan mode the per-step acquisition follows
`pluto_sdr_read_async`:

```
for each hop i:
    set RX LO (device_freqs[i])
    sleep after tune (PLUTO_SETTLE_DELAY_US = 0)
    for d in 1..PLUTO_DISCARD_COUNT: refill & discard
    refill useful buffer
    scan_callback -> scan_queue_push
```

In single mode, the same loop runs with `scan_count == 1`, and every buffer
goes to the worker. Each published waterfall line comes from exactly one FFT.
Continuous single mode uses four IIO kernel buffers so the device can keep
capturing while userspace extracts channels, converts samples, and queues the
preceding refill. A single IIO kernel block is reserved for scan/hop mode; using
it for stateful CIC capture can leave refill-boundary gaps upstream of the
software queue.
For decimated/CIC single mode, the async refill normally equals
`decim_hop * decim_factor` raw complex samples, giving one hardware refill per
steady-state displayed line. The refill is capped at
`SINGLE_DECIM_ASYNC_MAX_LEN` for very large line advances; capped cases are
visible in logs as `async < line samples`. `decim_hop == effective_fft_size`
when overlap is inactive. When the minimum waterfall-rate control is active,
`overlap_factor = effective_fft_size / decim_hop` is an integer power of two,
and adjacent FFT windows intentionally share decimated samples. Every completed
FFT is published or explicitly removed by the maximum-rate limiter; no
completion is overwritten or averaged into another line.
If an inner or outer recovered refill, or a short read, occurs, the worker
receives an ordered discontinuity flag, resets CIC state, and
skips the first warmed-up FFT frame rather than publishing a line across a
hidden sample discontinuity.

For compatible single-frequency view changes, each accepted raw complex sample
has an absolute index and a continuity epoch. The capture callback keeps
appending Q15 I/Q to a bounded ring while the worker temporarily stops accepting
old-generation queue entries. The worker snapshots exactly
`(effective_fft_size + CIC_STAGES) * decim_factor` samples ending at the handoff
index, initializes the new NCO/CIC/FFT generation, and publishes its completed
historical frame as `preview=1`. It then processes every indexed sample captured
after the handoff and atomically resumes the live queue at the same cursor. No
sample may be lost, duplicated, or joined across continuity epochs. The physical
LO is not rewritten and the scan thread is not restarted. Compatibility requires
the same single-mode hardware sample rate, RF bandwidth, converter/input path,
and analog coverage of the target DSP source.

If those conditions are not met, the backend restarts capture and may use the
older bounded preview bridge. That fallback never concatenates pre-restart and
post-restart samples in one FFT.

### 3.6 I/Q decoding

`pluto_sdr_refill_float` converts each `int16` sample to
`float` by dividing by `2048.0`. It tracks input peak magnitude and counts
clipped samples (raw `±2047/±2048` saturation) for the status report.
Converted to complex `float[2*i], float[2*i+1]` layout for the FFT.

### 3.7 Level normalization (summary; formulas in `SPECTRUM_CALC.MD`)

- Hann window coherent gain: `scale = 1 / sum(window)`.
- FFT magnitude: `|FFT(x_windowed)| × scale`.
- CIC DC gain: `1 / decim^3`.
- CIC runtime state: three bounded moving sums of length `decim`, one delay
  ring per stage, sampled every `decim` raw samples.
- CIC frequency droop: inverse normalised CIC response, clamped `[0.05, 8]`.
- Waterfall noise presentation: multiply only the packed display magnitude by
  `sqrt(90000 / Hann_ENBW)` and a Rayleigh peak-reducer factor for the actual
  raw-bin count in the screen pixel.
- Final transport: `display_mag → dB (20·log10) → uint8` mapped over
  `[−100, −20] dB`.
- Automatic waterfall mode keeps the same palette and SSE bytes, but expands
  its robust upper level target by `2.12` after the existing `1.2` headroom
  factor. Manual level controls are unchanged.

`magnitude_to_u8` performs the dB→byte mapping. This keeps a
coherently calibrated tone stable before display presentation, while the
additional ENBW factor keeps white-noise background brightness stable as
resolution narrows.

The raw buffers feeding CIC must remain contiguous. The scanner may drop
queued buffers in non-CIC paths, but single-frequency CIC mode waits for queue
space and only throttles after complete FFT lines or at the final publish-clock
guard. This preserves the stateful CIC filter and matches the cross-hardware
lesson from the older Fobos scanner: raw gaps before CIC corrupt the waterfall.

### 3.8 HTTP API

All responses are `application/json` (except HTML/text files) with permissive
CORS headers. `OPTIONS` preflight is answered with `204 No Content`.

| Method | Path | Body | Behavior |
|---|---|---|---|
| GET | `/` , `/index.html` | — | Serve `index.html` |
| GET | `/bands.ini` | — | Serve band overlay INI |
| GET | `/markers.ini` | — | Serve marker INI (empty template if missing) |
| GET | `/api/markers` | — | JSON list of markers + groups |
| POST | `/api/markers/save` | raw INI text | Validate & atomically write `markers.ini` |
| GET | `/api/status` | — | Full status snapshot (see §3.9) |
| GET | `/api/waterfall` | — | **SSE**: open stream, register fd, keep alive |
| POST | `/api/start` | JSON | Configure **and** start scan (see §3.10) |
| POST | `/api/stop` | — | Stop scan thread |
| POST | `/api/view` | JSON | Update viewport; hot-handoff compatible CIC views, otherwise restart if changed |
| POST | `/api/fft` | JSON | Set `fft_size` (`1024..65536`, power of two) |
| POST | `/api/gain` | JSON | Set `gain_mode` + `hardwaregain_db` |
| POST | `/api/rf-port` | JSON | Reserved backend RF-port plumbing; the current frontend is fixed to `A_BALANCED` |
| POST | `/api/rate` | JSON | Set `rate_limit_lps` (max waterfall line rate) |
| POST | `/api/min-rate` | JSON | Set `min_rate_lps` (min waterfall line rate) |
| POST | `/api/goto` | JSON | Persist Go To target (`goto_freq_hz`, `goto_target_zoom`, `goto_animate`, `goto_delay_s`) |

`/api/rate` accepts `min_rate_lps` together with `rate_limit_lps` for the
dual-handle rate slider. Pure maximum-rate changes update pacing in place,
including scan/hop mode; they do not restart the scan or retune Pluto.
Minimum-rate changes may restart single-frequency mode when the CIC overlap
geometry must change.

**Note:** `/api/start` is authoritative for RF settings. The client-supplied
`samplerate` (or alias `sample_rate`), `rf_bandwidth` (or alias
`rf_bandwidth_hz`), and `bw_ratio` are **ignored and forced** to the auto
profile (`61.44 MSPS`, `20 MHz`, `0.85`); `direct_sampling` is always reset to
`0`. The frontend shows those values as read-only status text instead of
ordinary user-editable controls.

### 3.9 `/api/status` response fields

| Field | Meaning |
|---|---|
| `device`, `sw_version` | Fixed device string; build date (`__DATE__`+`__TIME__`) |
| `hardware`, `firmware`, `serial`, `manufacturer`, `product` | Pluto identity (from IIO / USB serial) |
| `receiver_min_hz`, `receiver_max_hz` | Active Pluto RX LO limits in integer hertz |
| `receiver_range_source` | `"frequency_available"` or `"fallback"` |
| `receiver_range_note` | `"active Pluto receiver range"` |
| `scanning`, `device_present` | Run state flags |
| `auto_restart_on_reconnect`, `resume_scan` | Pending device reconnect and program-restart receive intents |
| `freq_start`, `freq_end` | Configured full band (Hz) |
| `configured_start_hz`, `configured_end_hz` | Alias of full band |
| `visible_start_hz`, `visible_end_hz` | Current viewport |
| `scan_start_hz`, `scan_end_hz` | Effective scan span after clamping |
| `second_if_hz`, `zero_if_guard_hz` | Source-center offset & zero-IF clearance |
| `samplerate`, `rf_bandwidth`, `bw_ratio` | Active hardware values |
| `hardware_samplerate` … `hardware_bw_ratio` | Selected profile values |
| `active_samplerate` … `active_bw_ratio` | Zoom-mode values shown in UI |
| `step_hz`, `steps`, `required_points` | Hop step, hop count, planning points |
| `mode`, `active_mode` | `"scan"` / `"single"` |
| `min_rate_lps`, `rate_limit_lps` | Waterfall rate budget |
| `rate_drop_factor`, `raw_line_rate` | Empirical boosted line-rate estimate & throttle |
| `true_line_rate` | Base FFT line cadence before minimum-rate overlap boosting |
| `minimum_rate_overlap`, `minimum_rate_limited`, `minimum_rate_achieved` | Min-rate overlap state; `minimum_rate_limited` is a legacy alias |
| `traffic_kbytes_s` | Compressed SSE traffic for one frontend stream (2 s window) |
| `bins_per_step`, `line_bins`, `raw_line_bins`, `display_bins` | Bin accounting |
| `view_id` | Monotonic id incremented on every view change |
| `fft_size`, `effective_fft_size` | Selected vs planner-chosen FFT |
| `decim_factor`, `decim_hop`, `overlap_factor` | Integer CIC factor, decimated-sample hop, and power-of-two overlap |
| `effective_input_samples` | Samples consumed per published line |
| `gain_mode`, `hardwaregain_db`, `rf_port` | Gain and backend RF-port state; the supported UI value is `A_BALANCED` |
| `rf_ports` | Backend-probed RF-port capability list reserved for later channel/port work |
| `rssi_db`, `input_peak`, `clipped_samples` | Live readbacks |
| `goto_freq_hz`, `goto_target_zoom`, `goto_animate`, `goto_delay_s` | Go To state |
| `sample_rates` | JSON array of supported rates |

### 3.10 `/api/start` request body (subset)

`freq_start`, `freq_end` (MHz), `converter_freq` (MHz), `samplerate`*,
`rf_bandwidth`*, `bw_ratio`*, `gain_mode`, `hardwaregain_db`, `rf_port`,
`fft_size`, `display_bins`, `min_rate_lps`, `rate_limit_lps`,
`visible_start_hz`, `visible_end_hz`. (*forced to auto profile, see §3.8.)

The current frontend always sends `rf_port="A_BALANCED"`. Its `Input` control
contains only `1 (A balanced)` and is disabled because the current scanner
supports one complex receiver channel. The backend retains probing and
`/api/rf-port` plumbing for development and future channel/port support; those
alternate values are not part of the supported frontend workflow.

`freq_start`/`freq_end` are air-frequency values in MHz. Receiver limits are
validated after applying `converter_freq`; invalid converted bands return
HTTP `400` without changing the persisted configured band.

The response echoes a full planning summary identical in shape to
`/api/status`, plus `status:"ok"|"error"`.

`POST /api/view` additionally returns `transition:"hot"` when a compatible
indexed-history handoff was queued, `transition:"restart"` when capture had to
restart, or `transition:"unchanged"` when no new view was applied.
Hot compatibility requires the same hardware profile and analog coverage plus
mutually overlapping old/new DSP centers; a large pan or Go To therefore
retunes instead of carrying a distant narrow-source tone through the CIC.

### 3.11 SSE waterfall row

```
event: line
data: {"view":<id>,"n":<line#>,"b":<total_steps>,"mode":"scan|single",
       "f0":<visible_start_hz>,"f1":<visible_end_hz>,
       "full_f0":<configured_start_hz>,"full_f1":<configured_end_hz>,
       "visible_start_hz":<start>,"visible_end_hz":<end>,
       "second_if_hz":<if_offset>,"zero_if_guard_hz":<guard>,
       "fq_err_correction_hz":<total_coordinate_correction>,
       "physical_center_hz":<actual_lo_center>,
       "dsp_center_hz":<post_nco_center>,"digital_mix_hz":<nco_shift>,
       "capture_epoch":<continuity_id>,
       "frame_sample_start":<inclusive_raw_index>,
       "frame_sample_end":<exclusive_raw_index>,
       "hot_replan_count":<completed>,"hot_replan_failures":<failed>,
       "raw_peak_hz":<interpolated_raw_fft_peak>,
       "live_capture_age_ms":<monotonic_age>,
       "live_first_input_ms":<delay>,"live_first_fft_ms":<delay>,
       "live_first_publish_ms":<delay>,
       "display_bins":<N>,"source_bins":<N>,"raw_source_bins":<N>,
       "effective_fft_size":<fft>,"decim_factor":<d>,"decim_hop":<h>,
       "overlap_factor":<o>,
       "minimum_rate_overlap":<0|1>,"minimum_rate_achieved":<0|1>,
       "true_line_rate":<lines_per_second>,
       "preview_interval_ms":<planned_playback_interval>,
       "line_interval_ms":<planned_steady_state_interval>,
       "waterfall_noise_scale":<enbw_display_factor>,
       "preview":<0|1>,"preview_age_ms":<ms>,
       "preview_sequence":<i>,"preview_count":<n>,
       "encoding":"u8b64","db":"<base64 packed uint8 row>"}
```

- `encoding:"u8b64"` means `db` is base64 for exactly `display_bins` packed
  `uint8` values, one per screen pixel. This avoids the 4-5x overhead of JSON
  decimal arrays while staying compatible with EventSource.
- Legacy rows with `d:[<u8>, ...]` are still accepted by the frontend.
- `source_bins` is intentionally set to `display_bins` (the published width).
- `raw_source_bins` exposes the pre-reduction FFT/CIC bin count for diagnostics.
- `raw_peak_hz` is the strongest visible pre-reduction bin with bounded
  three-point interpolation. Hidden source-span bins outside
  `visible_start_hz..visible_end_hz`, including zero-IF/DC artifacts, must not
  replace the carrier being inspected. It is diagnostic metadata for a known
  carrier, not a second display row or an RF tuning command.
- The `live_*_ms` fields are monotonic delays from live capture start. Preview
  rows report zero until fresh acquisition reaches that stage; they make a
  preview-to-live pause measurable without mixing the two sample streams.
- `view` is the `view_id`; the frontend ignores rows whose `view` is older than
  the one it requested, so zoom/pan changes never show stale data.
- `capture_epoch` identifies one uninterrupted physical sample stream.
  `frame_sample_start..frame_sample_end` is the exact raw-sample interval used
  by that FFT. Across a hot handoff the epoch and physical center stay fixed;
  frame ends advance by positive multiples of `decim_hop * decim_factor`.
- `minimum_rate_overlap=1` means the min-rate control shortened `decim_hop`
  while preserving FFT size and CIC decimation.
- `preview=1` identifies a row computed from compatible historical samples by
  the same NCO/CIC/Hann/FFT path. In a hot handoff it is the frame ending at the
  indexed boundary of the still-running capture; following `preview=0` rows
  continue that epoch. In the restart fallback, preview rows remain independent
  from the newly started live stream. `line_interval_ms` is the planned
  steady-state cadence; the frontend queues rows against one retained monotonic
  deadline so neither path produces a burst followed by a pause. Animated Go To
  accepts a matching current-resolution preview row and never stretches an
  older row into the new viewport.

---

## 4. Frontend: `index.html`

Single file: HTML structure + inline `<style>` + inline `<script>`. No
bundler, no framework, no npm runtime dependency.

### 4.1 Rendering

- **Spectrum scope** — 135 px `<canvas>`, redrawn from the latest SSE row;
  draws band overlays, grid, spectrum line, markers, and a peak dot.
- **Waterfall** — 600 px `<canvas>` using raw `ImageData`; newest row written
  at the top, history scrolled down by `copyWithin`. Retained in `wfRows[]`.
- **Ruler overlays** — two `<canvas>` layers (`#rulerOverlay`,
  `#scopeRulerOverlay`) drawn on top of the main canvases.
- **Frequency scale** — auto-ticked ruler below the scope with major/mid/minor
  ticks chosen for the current span (`chooseFrequencyStep`). Labels group Hz
  digits after the kHz group (`10489.500 300 MHz`), and a centred light-blue
  `<10Hz>`-style marker shows the major-tick distance.
  Cursor hover labels use the same hertz coordinate transform, but keep at
  least three MHz decimals even when major ticks are whole MHz. When the
  visible waterfall/spectrum width is below `100 kHz`, hover labels must show
  hertz digits as six MHz fractional digits grouped after the kHz part, e.g.
  `10489.500 300 MHz`; when adjacent major marks are `≤ 5 Hz`, they must show
  one additional digit for `0.1 Hz` pointer resolution.

### 4.2 Color map

A 256-entry RGB lookup table (`colorLUT`) maps the packed waterfall byte to a
blue→cyan→yellow→white gradient. Waterfall levels are adjustable (auto or
manual dual slider); auto levels use robust percentiles (8 % noise floor,
98.5 % signal ceiling) with exponential smoothing.

### 4.3 SSE client

`EventSource("/api/waterfall")`. Each `line` payload is validated:
- `display_bins` is the logical CSS width selected when the view was planned.
  Browser page zoom and DPR changes retain that stream and resample locally to
  the current canvas backing width; they do not restart the receiver.
- `encoding:"u8b64"`/`db` must decode to `display_bins` bytes, or legacy `d`
  must be a length-`display_bins` array of integers `0..255`.
- `f0 < f1` and finite.
- Cached previews are queued and rendered at `preview_interval_ms`; live rows
  use `line_interval_ms`, and the next presentation deadline survives a
  temporarily empty queue until the next row or view change. An early live row
  replaces unrendered cached rows rather than accumulating display latency.
- When the document is hidden, paced presentation is suspended and only the
  newest pending SSE row is retained. After the document becomes visible, a
  short settle interval collapses delayed EventSource callbacks before that row
  is rendered. Retained row timestamps are clamped to nondecreasing order, and
  time-mark boundaries ignore backward pairs. This prevents background-tab
  timer throttling from creating bursts or reordered wall-clock labels.

On stall (no line for 6× the expected line interval, clamped `5–30 s`) the
client reconnects. Heartbeat shows `disconnected` / `idle` / `scanning`.

### 4.4 Zoom, pan, Go To

- **Shift-wheel zoom** centred on the cursor (factor `~1.18` per step,
  coalesced to ≤8 steps per animation frame). The cursor anchor keeps the
  frequency under the pointer fixed.
- **Pan** — drag on the waterfall; the view shifts live using a captured
  pixel-preview so the backend is not retuned on every mouse move.
- **Zoom buttons / keys** — `+`/`=` zoom in, `−`/`_` zoom out, arrows pan,
  `0` resets to full band.
- **Zoom boundaries** — zoom-in requests at `1,000,000×` and zoom-out
  requests at `1×` are no-ops. They must not alter the URL viewport, request a
  backend view, or rematerialize the retained waterfall.
- **View coalescing** — ordinary zoom/pan updates the canvas immediately but
  debounce the backend retune by `VIEW_RETUNE_IDLE_DELAY_MS = 300 ms`
  (`scheduleViewUpdate`), preventing rapid restart loops.
- **Go To dialog** — target frequency + zoom (`1×..1,000,000×`), optional
  animation with delay (`0.2/0.5/1/2/3/5 s`). Non-animated Go To retunes
  immediately. Animated Go To requests a backend view at every step and waits
  for a matching current-resolution FFT/CIC row (a compatible preview or live
  row) so each step does not stretch the previous lower-resolution image.
- **Persistence** — the visible viewport is saved to `localStorage` and the URL
  hash (`#view=start-end`); restored on page load. Stop then Run preserves a
  valid URL hash viewport when it fits inside the typed Start/End band. A
  `resume_scan = 1` receive is auto-started after program restart; explicit
  Stop clears the intent and leaves the backend idle.

### 4.5 Rulers, markers, bands

- **Rulers** — Ctrl+drag on waterfall or scope; reports frequency width and,
  on the scope, dB height.
- **Markers** — Shift+left-click (or Ctrl/Alt+right-click) opens a marker
  dialog; saved to `markers.ini` on the server; drawn as yellow flags.
- **Bands** — loaded from `bands.ini`, drawn as coloured blocks on the scope.

### 4.6 Key frontend constants

| Constant | Value |
|---|---|
| `MAX_ZOOM_FACTOR` | 1,000,000 |
| `MIN_AIR_FREQ_START_MHZ` | 0 |
| `MIN_SCAN_SPAN_MHZ` | 0.1 |
| `MAX_WF_HISTORY_BINS` | 12,000,000 |
| `WF_REDRAW_ROWS_PER_FRAME` | 24 |
| `VIEW_RETUNE_IDLE_DELAY_MS` | 300 |
| `SSE_STALL_MIN_MS` / `MAX_MS` | 5,000 / 30,000 |
| `SPECTRUM_DB_FLOOR` / `DB_CEIL` | −100 / −20 |
| `RATE_VALUES` | `[0,1,2,5,10,20,50,100]` lines/s |
| `AUTO_RESTART_DELAY_MS` | 900 |

### 4.7 API surface used by the frontend

`apiGet("/api/status")`, `apiPost("/api/start", …)`, `apiPost("/api/stop")`,
`apiPost("/api/view", …)`, `apiPost("/api/fft", …)`,
`apiPost("/api/gain", …)`, `apiPost("/api/rate", …)`,
`apiPost("/api/min-rate", …)`, `apiPost("/api/goto", …)`,
`apiGet("/api/markers")`, `apiPost("/api/markers/save", …)`,
`fetch("/bands.ini")`, `fetch("/markers.ini")`. All POST bodies are JSON;
responses are parsed defensively (HTTP error → `{status:"error", ...}`).

---

## 5. Support files

| File | Purpose |
|---|---|
| `Makefile` | `gcc main.c -o pluto-scanner` (`-std=c99 -Wall -Wextra -liio -lm -pthread`) |
| `pluto-scanner.conf` | Local runtime state saved by backend (**untracked**) |
| `bands.ini` | Editable frequency band overlay definitions |
| `markers.ini` | Editable frequency marker definitions |
| `SPECTRUM_CALC.MD` | **Mathematical reference**: zoom table, RF profiles, FFT/CIC planning, level normalization, timing estimates |
| `AGENTS.md` | Agent (opencode) behavioural rules and coefficient formulas |
| `tools/` | Test and validation scripts (`*.sh`, `*.py`) |
| `images/` | README screenshots |

---

## 6. Tools, instruments & technologies

| Tool / technology | Role |
|---|---|
| **ADALM-Pluto** | Target SDR (AD9363 radio, Zynq FPGA, IIO interface) |
| **libiio** | IIO context, device attribute read/write, buffer management |
| **GCC + Make** | Build toolchain |
| **POSIX threads** | Scan thread, worker queue, watchdog, main loop |
| **libm** | FFT, CIC, log-domain magnitude math |
| **Berkeley sockets** | Bare TCP server + SSE (no HTTP library) |
| **Server-Sent Events** | Real-time waterfall row streaming |
| **HTML5 Canvas** | Spectrum scope, waterfall, ruler overlays |
| **EventSource API** | Frontend SSE consumer |
| **localStorage** | Viewport persistence across sessions |
| **Python 3** | Test harness scripts (`tools/*.py`) |
| **Selenium + geckodriver** | Browser UI tests (`tools/ui_browser_test.py`) |
| **Node.js** | Frontend JS syntax check (`make check` extracts `<script>` and runs `node --check`) |

The build supports a headless mode via the `PSEUDO_RANDOM_SAMPLE_SOURCE`
compile flag, which replaces the Pluto device with a deterministic
pseudo-random sample generator so the full pipeline (HTTP, FFT, CIC, SSE) can
be tested without hardware.

---

## 7. Data flow

```
User sets frequency band + zoom + gain
        │
        ▼
Frontend POSTs /api/start (or /api/view after zoom/pan)
        │
        ▼
Backend planner:
  - span = visible_end − visible_start
  - required_points = ceil(span / (rf_bw × bw_ratio))
  - mode = scan if required_points > 1, else single
  - scan:   choose FFT = clamp_pow2(display_bins·sr / span, 1024, 8192)
  - single: choose profile, minimum CSS-covering FFT, integer CIC
        │
        ▼
Backend configures Pluto (LO, samplerate, bw, gain) ── converter math:
    rx_lo = air − f_conv        (f_conv ≥ 0)
    rx_lo = |−f_conv − air|     (f_conv < 0, e.g. −530 MHz for FM band)
    reject before changing config if rx_lo would leave receiver_min_hz..receiver_max_hz
        │
        ▼
Acquisition:
  scan:   retune → discard 2 → refill 1 → Hann → FFT → stitch
  single: continuous stream → indexed Q15 history + worker queue
        │
        ▼
Single DSP: NCO → bounded CIC → Hann → FFT
Both modes: magnitude × coherent/CIC/noise scales → calibrated raw bins
        │
        ▼
publish_scan_line: peak-per-CSS-pixel reduce → uint8 display_bins → SSE
        │
        ▼
Frontend: validate row → update scope → scroll waterfall → relabel → autoscale
```

---

## 8. Runtime stability rules (from `AGENTS.md`)

- Shift-wheel / zoom buttons / pan update the canvas immediately but coalesce
  backend retune (`VIEW_RETUNE_IDLE_DELAY_MS = 300 ms`).
- Non-animated Go To retunes immediately; animated Go To retunes at every
  animation step and waits for a matching row before advancing.
- libiio may print `ERROR: READ ... -9` on cancelled buffers during
  stop/restart — treated as expected noise unless followed by
  `Closing SDR device after I/O error` or `Device reconnected`.
- A small number of `pluto_sdr_read_async failed: -1; retrying RX buffer`
  lines during stress is acceptable if the retry recovers.
- In single-frequency CIC mode, `CIC queue waited ...` means the processing
  queue blocked to preserve raw-buffer continuity instead of dropping samples
  before the decimator. Occasional waits are acceptable; repeated waits during
  a long integer-CIC run indicate worker backpressure that should be
  investigated.
- In single-frequency CIC mode, the end-of-scan `CIC samples:` diagnostic must
  show `accounting errors 0`. Nonzero short-read or refill-recovery counters
  mean the scanner reset CIC and skipped a warm-up frame to avoid publishing
  across a discontinuity.
- CIC queue items carry a block ordinal and logical source range. The worker
  must report no `CIC sample-order errors`. This proves ordering from callback
  acceptance through the worker queue; Pluto/libiio supplies no per-sample
  timestamp, so transport continuity is validated by multi-buffer capture,
  refill diagnostics, long live runs, and the synthetic spectral test.
- In ordinary CIC plans, `async` equals `decim_hop * decim_factor` and
  `kernel buffers` is `4`. When that hop exceeds `262144`, the logged `async`
  value is capped and one FFT advance is intentionally assembled across
  multiple contiguous refills.
- The separate synthetic-tone build must keep a clean bin-centred tone within
  a three-bin Hann main lobe at x2, x64, x256, x5581, and x7761. Periodic skip
  and duplicate controls must fail both the source-range and spectral-leakage
  checks. An unchanged physical tone may become off-bin after a DSP-center
  change; in that case the diagnostic checks expected peak position and Hann
  main-lobe width without applying the bin-centred `-70 dBc` sidelobe limit.
  If the expected tone bin is outside the extracted source, the spectral check
  is skipped because that FFT contains no in-source reference tone.
  The exact 453 Hz/630 Hz synthetic test runs at 1800 and 1346 logical pixels
  and must keep one capture epoch and avoid a scan-thread restart in both
  directions. `tools/live_principle7_check.py` applies the same identity,
  indexed-frame, first-row, and cadence checks to a five-minute Pluto run.
- Scan is stopped automatically after `FRONTEND_IDLE_STOP_MS = 20 s` of no
  frontend activity.
- The device is polled for reconnect every 5 s. A saved `resume_scan = 1`
  receive is resumed after program restart or delayed Pluto reconnect; explicit
  Stop clears the intent. The browser action button is labeled `Run`; the API
  endpoint remains `/api/start`.

---

## 9. Reference documents

- **`SPECTRUM_CALC.MD`** — complete mathematical specification: zoom table
  (`1×..1,000,000×`), RF profile selection, scan/single FFT/CIC planning
  formulas, line-rate estimates, and level normalization. This is the
  authoritative companion to the present file.
- **`AGENTS.md`** — agent behavioural rules, two-mode description, and machine-
  readable coefficient formulas.
- **`README.md`** — build / run / test instructions and the FM broadcast example.
- ADALM-Pluto specifications: <https://wiki.analog.com/university/tools/pluto/devs/specs>
- AD9363 data sheet: <https://www.analog.com/media/en/technical-documentation/data-sheets/ad9363.pdf>
- libad9361 filter notes: <https://github.com/analogdevicesinc/libad9361-iio>
