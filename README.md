# Pluto SDR Scanner with a Million Times Zoom

Browser-based spectrum and waterfall scanner for ADALM-Pluto using `libiio`.

The app keeps the original scanner UI model: a small C HTTP server on `localhost:8080`, a single-page `index.html` frontend, and live spectrum/waterfall rows streamed over Server-Sent Events. Hardware scan behavior is implemented as a host-side Pluto hop loop:

```text
set RX LO -> discard stale buffers -> refill useful buffer -> FFT -> publish row
```

## My Motivation

I want this project to explore new principles for SDR tools:

1. AI-first development.
2. A web interface that minimizes traffic.
3. A spectrum view with very large zoom: from gigahertz-wide full-screen spans down to hertz-per-pixel detail, one million times zoom.
4. Seamless merging of scan/hop mode and single-frequency reception, hidden from the user.
5. Waterfall speed limits expressed as a range from and to lines per second instead of tying behavior directly to FFT size.
6. Persistent waterfall history: when zooming or moving through frequencies, the waterfall is not cleared. It shows all recorded data that still applies, even when stretched. This is a known SDR UI principle, but it still needs better implementation so history remains useful across large zoom and frequency changes.
7. Low-latency resolution changes: very fine frequency resolution needs FFTs built from many seconds of samples. Traditional SDR programs can make the user wait several seconds after switching resolution before the first new waterfall line appears. This scanner reuses compatible samples already held in memory for the first preview lines, so the display responds quickly while live acquisition catches up.

## Defaults

The default scan/hop profile is:

```text
freq_start       = 70 MHz
freq_end         = 1000 MHz
sample_rate      = 61.44 MSPS
rf_bandwidth     = 20 MHz
fft_size         = 1024
rx_buffer        = 1024 complex samples
kernel_buffers   = 1
discard_buffers  = 2
gain_mode        = manual
hardware_gain    = 20 dB
waterfall_levels = auto
```

Single-frequency streams use four queued IIO kernel buffers. The extra blocks
keep continuous capture running while userspace extracts I/Q, converts samples,
and copies the preceding refill into the worker queue. Scan/hop mode retains
one block for low stale-data latency after each retune.

The scanner now accepts the unofficial extended Pluto tuning range of `70 MHz` to `6 GHz` for receiver-side frequency validation. Start/end controls are air-frequency values, not receiver-frequency limits; values above `6 GHz` are valid when `converter_freq` maps them into the Pluto receiver range. FM broadcast-band testing still needs a converter mapping when the air frequency lies outside that direct tuning envelope. With this app's converter convention, the stable hardware test setup used `converter_freq = -530 MHz`, mapping `88-108 MHz` air frequency to `422-442 MHz` receiver tuning.

No Pluto reflashing is required for this program. It works with the original
Analog Devices stock firmware and DATV firmware variants, as long as the normal
libiio devices and AD936x attributes remain available.

## Build

```sh
make
```

Dependencies:

- GCC
- GNU Make
- `libiio` headers and library
- POSIX threads
- `libm`
- Node.js for `make check`
- Python 3 for validation tools
- Firefox, geckodriver, and Selenium for browser validation tools

On Debian/Ubuntu-like systems:

```sh
sudo apt install build-essential libiio-dev nodejs python3 python3-pip
python3 -m pip install -r requirements.txt
```

## Run

```sh
PLUTO_URI=ip:pluto.local ./pluto-scanner
```

The binary defaults to `ip:pluto.local` when `PLUTO_URI` is not set. You can also pass the URI explicitly:

```sh
./pluto-scanner --uri ip:192.168.2.1
```

Open:

```text
http://localhost:8080
```

## QO-100 narrowband transponder

The screenshot below was captured from the live browser UI after one minute on the QO-100 narrowband transponder band.


![QO-100 narrowband waterfall](images/qo100-nb-transponder.png)

## UI Controls

- Start/end frequency and converter are air-frequency settings. Receiver limits are checked after converter conversion, so invalid bands are rejected instead of silently rewritten.
- Sample rate, RF bandwidth, and passband usage are auto-profiled for Pluto performance and shown read-only in the UI.
- RF bandwidth is kept strictly below sample rate in every auto profile.
- Waterfall rows are published as exactly one processed output bin per screen pixel; raw FFT/CIC bin counts are kept as debug metadata.
- The frontend sends `display_bins` with view/start requests so backend rows match the current canvas width.
- Passband usage still defines hop spacing internally as `rf_bandwidth * ratio`.
- Gain mode and hardware gain map to AD936x `gain_control_mode` and `hardwaregain`.
- FFT/CIC status shows the active backend plan used for the current zoom.
- Narrow single-frequency CIC mode preserves raw-buffer continuity before the
  decimator; throttling happens after complete FFT lines so the filter state is
  not corrupted by dropped raw buffers.
- Decimated single-frequency mode normally requests one Pluto refill per
  displayed line, so `FFT=65536 x2` should log matching `async` and
  `line samples` counts.
- In CIC mode the minimum waterfall-rate control preserves the FFT size and
  CIC decimation, then uses integer overlap on the decimated stream to increase
  line cadence. The status line shows this as `FFT=<size> x<decim>` plus an
  overlap factor when active.
- Compatible high-zoom view changes can draw cached historical preview rows
  before the first new live row arrives. Preview rows use the same CIC/Hann/FFT
  path and are not mixed with post-restart samples. This is especially useful
  when very fine resolution needs many seconds of samples for the next FFT and
  the new decimated stream has not filled that window yet.
- Recovered RX-buffer retries and short reads reset CIC before the first
  post-gap block. The first complete frame after that reset is discarded.
- Frequency-response compensation and legacy LNA/VGA/direct-sampling controls are not part of the Pluto UI.

Zoom, pan, Go To, rulers, waterfall levels, markers, and band overlays are preserved.

## Tests

Without starting the backend:

```sh
make check
tools/cic_stability_check.py
tools/cic_continuity_check.py
tools/cic_synthetic_signal_check.py
tools/min_rate_overlap_check.py
tools/cached_preview_check.py
tools/fft_level_normalization_check.py
```

With the backend running:

```sh
tools/http_smoke_test.sh
tools/headless_tester.py
tools/ui_browser_test.py
tools/phase3_browser_stress.py
tools/zoom_sweep.py --use-existing --freq-start-mhz 70 --freq-end-mhz 6000 --min-rate-lps 10 --rate-limit-lps 20 --out zoom-rate-matrix.md
tools/browser_zoom_matrix.py --output browser-zoom-matrix.json --freq-start-mhz 70 --freq-end-mhz 6000 --min-rate-lps 10 --rate-limit-lps 20 --settle-seconds 4
tools/fm_screenshot.py --output images/fm-broadcast-waterfall.png --wait-seconds 60
tools/frontend_random_validation.py --output frontend-random-validation.json
```

The browser tools require Firefox, geckodriver, and Selenium. Override the
Firefox binary with `FIREFOX_BIN=/path/to/firefox` when needed.

`tools/headless_tester.py` checks:

- wide scan/hop mode
- single-frequency mode
- FM broadcast-band workflow with `converter_freq=-530 MHz`

`tools/ui_browser_test.py` launches Firefox headless through Selenium and checks the visible Pluto controls, nonblank waterfall rendering, zoom, Go To, ruler interaction, and single-frequency transition.

`tools/phase3_browser_stress.py` launches Firefox headless and stress-tests full-band zooming, repeated `1x..1,000,000x` zoom ladders, and Go To with and without animation.

`tools/zoom_sweep.py` measures actual SSE waterfall line cadence over the regular `1, 2, 5` zoom ladder. `tools/browser_zoom_matrix.py` repeats the same ladder through the browser in manual and animated Go To paths.

`tools/frontend_random_validation.py` randomly exercises zoom, pan, Go To, sliders, ruler/marker dialog paths, stop/start, waterfall rendering, and verifies scan/hop plus single-frequency FFT/CIC/line-rate planning through `/api/status`.

`tools/cic_synthetic_signal_check.py` builds a separate no-hardware test binary
and sends a continuous bin-centred complex tone through the production queue,
CIC, Hann, FFT, display-reduction, and SSE path at x2, x64, and x256. Periodic
skip and duplicate controls must be reported as sample-order errors and widened
spectra. The normal `pluto-scanner` binary always uses Pluto input.

For CIC changes, also run a live narrow-span `FFT=65536 x2` session for several
minutes when Pluto hardware is available, and a deeper `x64` span when testing
carrier continuity. Watch the backend log for `CIC queue waited ...`,
`CIC samples:`, `CIC sample-order errors`, repeated read retries, watchdog
cancels, or hard reconnect
messages; the bounded CIC and continuity unit checks do not replace that
long-run hardware pass.

## Files

- `main.c` - C backend, HTTP API, libiio Pluto control, FFT/waterfall pipeline.
- `index.html` - frontend UI.
- `bands.ini` - editable band overlays.
- `markers.ini` - editable frequency markers.
