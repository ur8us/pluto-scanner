#!/usr/bin/env python3
import json
import math
import sys


CIC_STAGES = 3
NOISE_REFERENCE_ENBW_HZ = 90_000.0


def hann(size):
    return [
        0.5 * (1.0 - math.cos(2.0 * math.pi * i / (size - 1)))
        for i in range(size)
    ]


def coherent_window_gain(size):
    return sum(hann(size))


def equivalent_noise_bandwidth(size, samplerate):
    """Return Hann ENBW in hertz for a complex FFT input stream."""
    window = hann(size)
    window_sum = sum(window)
    return samplerate * sum(value * value for value in window) / (window_sum * window_sum)


def waterfall_noise_scale(size, samplerate):
    """Return display-only factor that maps one bin's noise to the 90 kHz reference."""
    enbw = equivalent_noise_bandwidth(size, samplerate)
    return min(math.sqrt(NOISE_REFERENCE_ENBW_HZ / enbw), 1024.0)


def rayleigh_peak_median(count):
    """Return the exact median of a peak over count unit-Rayleigh magnitudes."""
    if count <= 1:
        return math.sqrt(2.0 * math.log(2.0))
    return math.sqrt(-2.0 * math.log(1.0 - math.exp(math.log(0.5) / count)))


def peak_reducer_noise_scale(count):
    """Return the one-bin-relative presentation scale for max-per-pixel reduction."""
    return rayleigh_peak_median(1) / rayleigh_peak_median(max(1, count))


def cic_gain(decim, freq_hz, raw_samplerate):
    if decim <= 1 or abs(freq_hz) < 1.0e-12:
        return 1.0
    x = math.pi * freq_hz / raw_samplerate
    den = decim * math.sin(x)
    if abs(den) < 1.0e-18:
        return 1.0
    return abs(math.sin(decim * x) / den) ** CIC_STAGES


def cic_weight(decim, freq_hz, raw_samplerate):
    gain = max(cic_gain(decim, freq_hz, raw_samplerate), 0.05)
    return min(1.0 / gain, 8.0)


def check_close(errors, label, actual, expected=1.0, tol=1.0e-6):
    if abs(actual - expected) > tol:
        errors.append(f"{label}: expected {expected:.9f}, got {actual:.9f}")


def main():
    errors = []
    fft_rows = []
    for size in (1024, 2048, 8192, 65536):
        scale = 1.0 / coherent_window_gain(size)
        amplitude = coherent_window_gain(size) * scale
        fft_rows.append({"fft": size, "window_scale": scale, "amplitude": amplitude})
        check_close(errors, f"fft {size} coherent amplitude", amplitude)

    noise_rows = []
    # (raw Pluto rate, CIC decimation, FFT). The FFT sees raw_rate / decim.
    # These cover scan, raw single, and the deepest documented CIC plans.
    for raw_rate, decim, size in (
        (61_440_000.0, 1, 1024),
        (61_440_000.0, 1, 8192),
        (20_000_000.0, 1, 8192),
        (4_000_000.0, 1, 65536),
        (4_000_000.0, 2, 65536),
        (4_000_000.0, 64, 65536),
        (4_000_000.0, 128, 65536),
    ):
        fft_rate = raw_rate / decim
        window = hann(size)
        window_sum = sum(window)
        # Unit white-noise power spectral density has per-sample variance Fs.
        # Coherent normalization alone yields sqrt(ENBW) bin magnitude.
        raw_noise_magnitude = math.sqrt(fft_rate * sum(v * v for v in window)) / window_sum
        enbw = equivalent_noise_bandwidth(size, fft_rate)
        presentation = waterfall_noise_scale(size, fft_rate)
        normalized_noise = raw_noise_magnitude * presentation
        noise_rows.append(
            {
                "raw_samplerate": raw_rate,
                "decim": decim,
                "fft": size,
                "fft_samplerate": fft_rate,
                "enbw_hz": enbw,
                "waterfall_noise_scale": presentation,
                "normalized_noise_magnitude": normalized_noise,
            }
        )
        check_close(
            errors,
            f"noise density sr={raw_rate} decim={decim} fft={size}",
            normalized_noise,
            math.sqrt(NOISE_REFERENCE_ENBW_HZ),
            tol=1.0e-5,
        )

    reducer_rows = []
    for raw_bins in (1, 2, 3, 8, 32, 256):
        median = rayleigh_peak_median(raw_bins)
        scale = peak_reducer_noise_scale(raw_bins)
        reducer_rows.append(
            {
                "raw_bins_per_pixel": raw_bins,
                "peak_median": median,
                "peak_reducer_noise_scale": scale,
                "normalized_peak_median": median * scale,
            }
        )
        check_close(
            errors,
            f"peak reducer raw bins={raw_bins}",
            median * scale,
            rayleigh_peak_median(1),
            tol=1.0e-12,
        )

    cic_rows = []
    raw_samplerate = 61_440_000.0
    for decim in (2, 32, 512, 1024, 2048, 4096, 5581, 7761, 65536):
        decimated_rate = raw_samplerate / decim
        for fraction in (0.0, 0.125, 0.25, 0.45):
            freq = decimated_rate * fraction
            compensated = cic_gain(decim, freq, raw_samplerate) * cic_weight(
                decim, freq, raw_samplerate
            )
            cic_rows.append(
                {
                    "decim": decim,
                    "freq_fraction_of_decimated_rate": fraction,
                    "cic_dc_scale": 1.0 / float(decim ** CIC_STAGES),
                    "cic_weight": cic_weight(decim, freq, raw_samplerate),
                    "compensated_amplitude": compensated,
                }
            )
            check_close(
                errors,
                f"cic decim {decim} fraction {fraction} compensated amplitude",
                compensated,
                tol=1.0e-5,
            )

    result = {
        "status": "ok" if not errors else "error",
        "noise_reference_enbw_hz": NOISE_REFERENCE_ENBW_HZ,
        "fft": fft_rows,
        "noise": noise_rows,
        "peak_reducer": reducer_rows,
        "cic": cic_rows,
    }
    if errors:
        result["errors"] = errors
    print(json.dumps(result, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
