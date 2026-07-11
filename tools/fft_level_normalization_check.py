#!/usr/bin/env python3
import json
import math
import sys


CIC_STAGES = 3


def hann(size):
    return [
        0.5 * (1.0 - math.cos(2.0 * math.pi * i / (size - 1)))
        for i in range(size)
    ]


def coherent_window_gain(size):
    return sum(hann(size))


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

    cic_rows = []
    raw_samplerate = 61_440_000.0
    for decim in (2, 32, 4096):
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

    result = {"status": "ok" if not errors else "error", "fft": fft_rows, "cic": cic_rows}
    if errors:
        result["errors"] = errors
    print(json.dumps(result, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
