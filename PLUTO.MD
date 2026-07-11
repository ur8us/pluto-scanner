# ADALM-Pluto Reference

## Overview

ADALM-Pluto (PlutoSDR) is an ADI SDR platform based on the AD9363 transceiver + Xilinx Zynq Z-7010 FPGA. Runs embedded Linux from ramfs. Communicates with host via USB gadget Ethernet (default IP `192.168.2.1`) or network.

## Firmware Compatibility

`pluto-scanner` does not require reflashing the Pluto. The original Analog
Devices stock firmware and DATV firmware variants are supported as long as the
standard libiio devices (`ad9361-phy`, `cf-ad9361-lpc`) and AD936x attributes
are available.

---

## IIO API (libiio)

The primary control API is **libiio** — IIO (Industrial I/O) subsystem over network.

### Key Devices

| Device | Purpose |
|---|---|
| `ad9361-phy` | Transceiver control (gains, filters, LO, calibration) |
| `cf-ad9361-lpc` | RX DMA capture (receive data stream) |
| `cf-ad9361-dds-core-lpc` | TX DMA / DDS (transmit data stream) |
| `xadc` | On-chip Xilinx ADC (temp, voltages) |

### Connection

```c
struct iio_context *ctx = iio_create_context_from_uri("ip:pluto.local");
```

URI formats: `ip:<hostname>`, `ip:<ip>`, `usb:`, `local:`.

### Device Naming (from this codebase)

```c
struct iio_device *phy   = iio_context_find_device(ctx, "ad9361-phy");
struct iio_device *rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
```

### Channel Naming

```c
// RX control (AD9361)
struct iio_channel *phy_rx0 = iio_device_find_channel(phy, "voltage0", false);  // input
struct iio_channel *rx_lo   = iio_device_find_channel(phy, "altvoltage0", true); // output (LO)

// RX data stream
struct iio_channel *rx_i = iio_device_find_channel(rxdev, "voltage0", false);
struct iio_channel *rx_q = iio_device_find_channel(rxdev, "voltage1", false);
```

### Reading / Writing Attributes

**Channel attribute (read string):**
```c
int n = iio_channel_attr_read(ch, "attr_name", buf, sizeof(buf)-1);
int n = iio_channel_attr_read_longlong(ch, "attr_name", &ll);
int n = iio_channel_attr_read_double(ch, "attr_name", &dd);
```

**Channel attribute (write):**
```c
int rc = iio_channel_attr_write_longlong(ch, "attr_name", value);
int rc = iio_channel_attr_write_double(ch, "attr_name", value);
int rc = iio_channel_attr_write(ch, "attr_name", "string_value");
```

**Device attribute:**
```c
int n = iio_device_attr_read(dev, "attr_name", buf, sizeof(buf)-1);
int n = iio_device_attr_write(dev->phy, "attr_name", buf);
```

### Common AD9361 Attributes

**`voltage0` (RX channel):**

| Attribute | Type | Description |
|---|---|---|
| `sampling_frequency` | long long | RX sample rate (Hz) |
| `rf_bandwidth` | long long | RX RF bandwidth filter (Hz) |
| `hardwaregain` | double | Gain (dB, range ~0..74.5) |
| `gain_control_mode` | string | `manual`, `slow_attack`, `fast_attack` |
| `rssi` | double | Received signal strength (dB) |
| `rf_port_select` | string | `A_BALANCED`, `B_BALANCED`, `C_BALANCED` |

**`altvoltage0` (TX LO channel):**

| Attribute | Type | Description |
|---|---|---|
| `frequency` | long long | RX LO frequency (Hz) |

**Device attributes (`ad9361-phy`):**

| Attribute | Type | Description |
|---|---|---|
| `xo_correction` | long long | Current XO frequency (Hz). Range `[39992000 1 40008000]`. Fine-tunes internal TCXO. **Does NOT select external clock.** |
| `xo_correction_available` | string | `[min step max]` format |
| `calib_mode` | string | `auto`, `manual`, `manual_tx_quad`, `tx_quad`, `rf_dc_offs`, `rssi_gain_step` |
| `calib_mode_available` | string | Available calibration modes |
| `ensm_mode` | string | `fdd`, `sleep`, `wait`, `alert`, `pinctrl` |
| `rx_path_rates` | string | Readback: `BBPLL:... ADC:... R2:... R1:... RF:... RXSAMP:...` |
| `tx_path_rates` | string | Readback: `BBPLL:... DAC:... T2:... T1:... TF:... TXSAMP:...` |
| `filter_fir_config` | string | FIR config readback |
| `multichip_sync` | string | Multichip sync (can't read on Pluto) |
| `gain_table_config` | string | Gain table (can't read on Pluto) |
| `dcxo_tune_coarse` | string | Coarse DCXO tune (can't read on Pluto Rev.C) |
| `dcxo_tune_fine` | string | Fine DCXO tune (can't read on Pluto Rev.C) |

### RX Data Buffer

```c
// Create buffer
struct iio_buffer *rxbuf = iio_device_create_buffer(rxdev, samples, false);

// Refill
ssize_t n = iio_buffer_refill(rxbuf);

// Access I/Q samples
ptrdiff_t step = iio_buffer_step(rxbuf);
void *data = iio_buffer_first(rxbuf, rx_i);
// Interleaved I0 Q0 I1 Q1 ... as 12-bit sign-extended to 16-bit ints
```

---

## External / Internal Clock Control

Clock source selection is done through **U-Boot environment variables** on the Pluto itself (SSH into the device). The `xo_correction` IIO attribute is only for fine-tuning the internal TCXO ±80 kHz and does NOT select the clock source.

### SSH Access

```
$ ssh root@pluto.local   # password: analog
```

### Clock Source Selection

**Check current state:**
```sh
fw_printenv refclk_source
```

**Enable external clock (Rev.C):**
```sh
fw_setenv refclk_source external
fw_setenv ad936x_ext_refclk_override "40000000"
pluto_reboot reset
```

**Switch back to internal TCXO (Rev.C):**
```sh
fw_setenv refclk_source internal
pluto_reboot reset
```

**Rev.B (no GPIO mux — external clock is direct wired):**
```sh
fw_setenv ad936x_ext_refclk "40000000"
fw_setenv xo_correction 40000000
fw_setenv qspiboot_extraenv true
pluto_reboot reset
```

### How It Works (Rev.C)

The U-Boot boot script (`adi_loadvals`) patches the device tree at boot time based on environment variables:

| Env var | Effect |
|---|---|
| `refclk_source=internal` | Removes `clock_extern_en` GPIO node → internal TCXO active |
| `refclk_source=external` | Removes `clock_internal_en` GPIO node → external clock active |
| `ad936x_ext_refclk_override=<freq>` | Patches `/clocks/clock@0/clock-frequency` in device tree |

GPIO line 954 (`clock_extern_en`) is hogged as `output/high` when external clock is selected.

### Verifying Which Clock Is Active

**Check device tree GPIO nodes (SSH):**
```sh
# External clock enabled:
ls /proc/device-tree/amba/gpio@e000a000/clock_extern_en   # exists
ls /proc/device-tree/amba/gpio@e000a000/clock_internal_en # removed

# Internal clock enabled:
ls /proc/device-tree/amba/gpio@e000a000/clock_extern_en   # removed
ls /proc/device-tree/amba/gpio@e000a000/clock_internal_en # exists
```

**Check dmesg:**
```sh
dmesg | grep clock_extern_en
# "GPIO line 954 (clock_extern_en) hogged as output/high" = external active
```

**Check `xo_correction` (libiio):**
```sh
# Read via libiio:
/tmp/check_xo  # custom program: iio_device_attr_read(dev, "xo_correction", ...)
# Via SSH:
cat /sys/bus/iio/devices/iio:device0/xo_correction
```
Note: `xo_correction` reports the frequency fed to the AD9361 (always 40 MHz whether internal or external). It cannot distinguish the source — it only confirms the clock frequency is correct.

### Frequency Notes

- Internal TCXO: 40 MHz
- External clock: 10–80 MHz (practical), but below 20 MHz is unreliable (PLL stability issues)
- 40 MHz external is most common (same as internal)
- The `ad936x_ext_refclk_override` (Rev.C) or `ad936x_ext_refclk` (Rev.B) sets the expected external frequency

### Reboot Command

```sh
pluto_reboot reset
```

---

## AD9361 Register Map & Low-Level Control

For register-level access, use the `ad9361-phy` debug attribute interface or direct SPI:
```sh
# Via debugfs on Pluto:
cat /sys/kernel/debug/iio/iio:device0/direct_reg_access
echo "0xREG 0xVAL" > /sys/kernel/debug/iio/iio:device0/direct_reg_access
```

Key registers:
- Reg 0x05: RX LO frequency (indirect, via synth registers)
- Reg 0x0A–0x0C: RX gain control
- Reg 0x3E: ENSM mode control

---

## Reference Links

- [AD9361 Reference Manual (UG-570)](https://www.analog.com/media/en/technical-documentation/user-guides/AD9361_Reference_Manual_UG-570.pdf)
- [ADALM-Pluto Wiki (developer)](https://wiki.analog.com/university/tools/pluto/developers)
- [ADALM-Pluto Booting Wiki](https://wiki.analog.com/university/tools/pluto/devs/booting)
- [AMSAT-DL External Clock Guide](https://wiki.amsat-dl.org/doku.php?id=en:tricks:pluto_extclk)
- [libiio on GitHub](https://github.com/analogdevicesinc/libiio)
- [pluto-scanner project](/) (this repo)
