# ADS1256 Library for Linux SBCs

C library for the TI ADS1256 24-bit ADC over Linux `spidev` (Orange Pi, Raspberry Pi and other single-board computers).

## Features

- Any input combination: 4 differential pairs, 8 single-ended inputs against AINCOM, or any other pair
- Data rates 2.5 SPS to 30 kSPS, PGA gain 1 to 64, optional input buffer
- Optional DRDY pin on GPIO: continuous streaming (RDATAC) and exact calibration waits; without it the library polls the STATUS register
- Multi-input scan: each input restarted with SYNC, so every value is settled and belongs to its input
- Raw signed 24-bit codes, conversion to volts on request
- Device handle owned by the caller: no global state, any number of devices
- Gain, data rate and buffer changes recalibrate automatically
- Timing per datasheet (TI SBAS288K, fCLKIN = 7.68 MHz), including t6 and t10 around CS

## Hardware Connection

```
ADS1256   Orange Pi 5 / Raspberry Pi header
-------------------------------------------
CS        CE0   (pin 24)
DOUT      MISO  (pin 21)
DIN       MOSI  (pin 19)
SCLK      SCLK  (pin 23)
GND       GND   (any GND pin)
5V        5V    (pin 2)
DRDY      any GPIO, optional
```

Pull SCLK down and CS up (about 10 kΩ) so the ADC sees an idle bus while the board boots and the SPI pins float: a glitch on SCLK shifts the command bits, and the datasheet wants SCLK held low when idle.

### DRDY pin (optional)

Without DRDY, the library polls the STATUS register and waits fixed datasheet times (+10 %) after calibration, because no command may be sent before it finishes. After reset it always waits 10 ms. Each streamed sample then costs a STATUS poll plus an RDATA command, and a lost conversion can't be noticed: on the Orange Pi 5 streaming is reliable only at 30 SPS or less (see [Streaming](#streaming)).

With DRDY on a GPIO, the library waits for its falling edge through the kernel GPIO character device (no extra library needed). It streams with RDATAC and knows exactly when calibration ends.

To use it, set `drdy_chip` and `drdy_line` in the configuration. Find the chip and line of your header pin with `sudo gpioinfo`. GPIO chips are root-only by default. To allow your user, add a udev rule, e.g. `/etc/udev/rules.d/99-gpio.rules`:

```
SUBSYSTEM=="gpio", KERNEL=="gpiochip*", GROUP="gpio", MODE="0660"
```

Then run `sudo groupadd -f gpio && sudo usermod -a -G gpio $USER`, reload udev rules (or reboot) and log in again.

## Platform Notes

The library uses only kernel services, so timing works the same on all boards: `CLOCK_MONOTONIC` and `clock_nanosleep` for delays and timeouts, kernel timestamps of DRDY edges from the same clock, and spidev `delay_usecs` for the t6 and t10 delays. The ADC clock (7.68 MHz) comes from the crystal on the ADS1256 module, not from the board. Short delays (4-20 us) may last about 60 us because of kernel timer slack; they are minimum waits, so this only costs a little speed.

### Raspberry Pi 4

- **Kernel 5.10 or newer** (Raspberry Pi OS Bullseye or Bookworm). The GPIO uAPI v2 header is needed at compile time even without the DRDY pin, so Buster (4.19) does not work.
- **Use SPI0** (`/dev/spidev0.0`), enabled by `dtparam=spi=on` in `/boot/config.txt` (`/boot/firmware/config.txt` on Bookworm). The auxiliary SPI1 (`/dev/spidev1.x`) does not work in SPI mode 1, which the ADS1256 needs.
- **DRDY pin**: `/dev/gpiochip0`, line = BCM GPIO number. For example DRDY on GPIO17 (header pin 11): `./ads1256 /dev/gpiochip0 17`.
- **Permissions**: `/dev/gpiochip*` belongs to group `gpio` and `/dev/spidev*` to group `spi`, and the default user is in both, so no udev rule is needed.
- **SCLK** is the 500 MHz core clock divided by an even number and rounded down: 1 MHz is exact, 1.92 MHz becomes about 1.908 MHz.
- **Waveshare High-Precision AD/DA board**: as far as known it has the ADS1256 CS on GPIO22 instead of CE0, and DRDY on GPIO17. Spidev drives only CE0, so CS on GPIO22 needs a device tree overlay with `cs-gpios`. A module wired as above (CS on CE0, pin 24) needs nothing.
- **Speed**: not measured. Expect streaming limits similar to or lower than on the Orange Pi 5 (see below).

### Orange Pi 5

- Enable SPI with a device tree overlay (`orangepi-config` or the `overlays=` line in `/boot/orangepiEnv.txt`).
- **DRDY pin**: Rockchip pin `GPIOx_yz` is `/dev/gpiochipx`, line `y * 8 + z` with A=0, B=1, C=2, D=3 (e.g. GPIO1_C6 is `/dev/gpiochip1`, line 22). Check with `sudo gpioinfo`.
- `/dev/gpiochip*` is root-only by default, see the udev rule above.
- **Measured streaming limits** (Orange Pi OS, kernel 6.1 with `PREEMPT_VOLUNTARY`, `spi4-m0-cs1-spidev` overlay, DRDY on GPIO1_A3):
  - With the DRDY pin, `read_stream()` reported `ADS1256_ERROR_OVERRUN` within a few hundred samples even at 1000 SPS, at both 1 MHz and 1.92 MHz SCLK. Waking up from `poll()` occasionally takes over 1 ms. Pinning to a Cortex-A76 core (`taskset -c 4-7`) did not make it reliable.
  - In a test build that busy-waits for the DRDY edge, 1000 and 2000 SPS ran clean on core 7. At 3750 to 15000 SPS overruns remained even with real-time priority (`chrt -f 50`), because delays of 100 to 300 us come from the kernel itself.
  - One read through spidev takes about 60 us, longer than the 33 us period at 30 kSPS.
  - `read_stream()` at SCLK 1 MHz, 2 minutes per rate (1000 and 500 SPS without DRDY: 5 s). With DRDY the stream was restarted after each overrun; without DRDY lost conversions were counted from the stream duration (after SYNC, n samples take n conversion periods, each lost one adds a period), checked against DRDY edges:

    | SPS | With DRDY: overruns (reported) | Without DRDY: lost conversions (not reported) |
    |---|---|---|
    | 1000 | 1086 in 2 min, longest clean run 1169 samples | 21 % (785 SPS delivered) |
    | 500 | 600 in 2 min, longest clean run 1194 samples | 14 % (430 SPS delivered) |
    | 100 | 12 in 2 min | 52 of 12000 |
    | 60 | 2 in 2 min | 9 of 7200 |
    | 50 | not measured | 5 of 6000 |
    | 30 | 0 | 0 |

  - The cause is the process being delayed by 10 to 13 ms now and then: a 7 ms sleep ended after 20 ms, a single STATUS read through spidev took 9 ms. Lower SCLK does not help (100 SPS without DRDY for 60 s: 26, 38 and 37 lost conversions at 1 MHz, 500 kHz and 250 kHz).
  - `read()` and `scan()` restart the conversion and are not affected; a delay only makes them slower.

## Requirements

No libraries: only the Linux `spidev` driver and the GPIO character device (uAPI v2, kernel 5.10 or newer, used only with the DRDY pin).

## Build

```bash
make                # Example program ./ads1256
make lib            # Static library libads1256.a
make test           # Hardware-free test with an emulated ADS1256
make hwtest         # Self-check on the connected ADS1256, see below
make install        # libads1256.a to ~/lib, ads1256_lib.h to ~/include
```

`make hwtest` (or `make hwtest HWARGS="/dev/gpiochip1 22"` for another DRDY line) needs the ADC with DRDY wired; inputs may float. In about two minutes it checks what the emulator can't: the chip leaves RDATAC after every stream and a stream takes no extra conversion period, `ads1256_open()` recovers after a process was killed mid-stream (also at 30 kSPS), the fixed calibration waits used without DRDY are long enough (prints the margin per data rate), and `ads1256_scan()` never returns a neighbouring input's data (alternating AIN0-AINCOM / AINCOM-AIN0 must read +V / -V, which floating inputs give).

Link your program with `-lads1256`, or just compile `ads1256_lib.c` with it.

## Usage

### Single reading

```c
#include <stdio.h>
#include "ads1256_lib.h"

int main(void)
{
    ads1256_config_t cfg = {
        .spi_device = "/dev/spidev0.0",
        .spi_speed_hz = 1000000,          /* Max 1.92 MHz */
        .drdy_chip = NULL,                /* Or "/dev/gpiochipN" + .drdy_line */
        .v_ref = 2.5,
        .drate = ADS1256_DRATE_100,
        .gain = ADS1256_GAIN_1,
        .pos = ADS1256_AIN0, .neg = ADS1256_AIN1,
        .buffer = false,
        .timeout_ms = 1000,
    };
    ads1256_t adc;
    int32_t raw;

    int result = ads1256_open(&adc, &cfg);
    if (result != ADS1256_OK) {
        fprintf(stderr, "%s\n", ads1256_strerror(result));
        return 1;
    }
    if (ads1256_read(&adc, &raw) == ADS1256_OK)
        printf("AIN0-AIN1: %.6f V\n", ads1256_to_volts(&adc, raw));

    ads1256_close(&adc);
    return 0;
}
```

`ads1256_read()` restarts the conversion (SYNC + WAKEUP) and waits for settled data, so the result always matches the current input, gain and data rate.

### Single-ended inputs and scanning

```c
ads1256_set_input(&adc, ADS1256_AIN3, ADS1256_AINCOM);   /* AIN3 against AINCOM */

uint8_t inputs[3][2] = {
    { ADS1256_AIN0, ADS1256_AINCOM },
    { ADS1256_AIN1, ADS1256_AINCOM },
    { ADS1256_AIN6, ADS1256_AIN7 },
};
int32_t values[3];
ads1256_scan(&adc, inputs, 3, values);
```

The scan restarts the conversion for each input with SYNC, so the first result is already settled (single-cycle settling). It reads each result before selecting the next input. The datasheet's input cycling (table 14) selects the next input first and reads while it converts, which is faster, but a Linux delay longer than the settling time then returns the next input's data as the previous one: on the Orange Pi 5 this hit 3.8 % of reads at 30 kSPS and 0.13 % at 1000 SPS. Reading first costs 2-20 % scan speed (100-30000 SPS). The last pair stays selected afterwards.

### Streaming

```c
int32_t samples[1000];
size_t count;                                /* Valid samples, also on error */
ads1256_read_stream(&adc, samples, 1000, &count);
```

With DRDY wired this uses RDATAC and returns `ADS1256_ERROR_OVERRUN` when the program can't keep up: a conversion was skipped, or a sample was read so late that the next update could overwrite it (judged by kernel timestamps of the DRDY edges). Linux is not a real-time system: an occasional delay longer than one conversion period is enough to lose a conversion. Without the pin, lost conversions are not detected.

Recommendations from the [measured limits on the Orange Pi 5](#orange-pi-5) (stock kernel, 2-minute runs; other boards and longer runs may differ):

- **Without DRDY**: stream at **30 SPS or less**. Above that, conversions are lost without notice (already at 50 SPS, about 1 in 230 at 100 SPS, 14-21 % at 500-1000 SPS). `ads1256_read()` and `ads1256_scan()` work at any data rate, only slower when delayed (the one exception seen: 3 of 96360 scanned reads at 30 kSPS returned a neighbouring input's data, cause unknown; none at 3750 SPS and below).
- **With DRDY**: 30 SPS or less ran clean. At 60-100 SPS expect an overrun every 10 s to 1 minute, at 500-1000 SPS on average every 100 samples (clean runs of at most about 1200). Losses are always reported: check the return value, keep the `count` valid samples and restart the stream. Clean streams above 30 SPS need a real-time setup: a test build that busy-waits for DRDY ran 1000-2000 SPS clean on one core (see the platform notes).
- If every conversion matters, use DRDY and a data rate where your board ran clean for the whole measurement time you need.

## API

| Function | Description |
|---|---|
| `ads1256_open(dev, cfg)` | Open SPI (+ GPIO), reset, configure, self-calibrate |
| `ads1256_close(dev)` | Close file descriptors (call before reopening the handle) |
| `ads1256_set_input(dev, pos, neg)` | Select inputs `ADS1256_AIN0..7` / `ADS1256_AINCOM` |
| `ads1256_set_gain(dev, gain)` | PGA gain 1-64, recalibrates |
| `ads1256_set_drate(dev, drate)` | Data rate, recalibrates |
| `ads1256_set_buffer(dev, on)` | Input buffer, recalibrates |
| `ads1256_calibrate(dev, cmd)` | SELFCAL, SELFOCAL, SELFGCAL, SYSOCAL or SYSGCAL |
| `ads1256_read(dev, &raw)` | One fresh conversion |
| `ads1256_read_stream(dev, raw, n, &count)` | n consecutive conversions, count of valid ones |
| `ads1256_scan(dev, inputs, n, raw)` | One conversion of each input pair, last pair stays selected |
| `ads1256_to_volts(dev, raw)` | Code to volts: `raw * 2 * v_ref / gain / 2^23` |
| `ads1256_sps(drate)` | Data rate in SPS |
| `ads1256_read_register` / `ads1256_write_register` | Low-level register access; bypasses `dev->cfg`, so use the setters for inputs, gain and data rate |
| `ads1256_strerror(code)` | Error text |

`dev->cfg` always holds the current settings. All functions return `ADS1256_OK` (0) or a negative error code: `ADS1256_ERROR_PARAMETER`, `ADS1256_ERROR_COMMUNICATION`, `ADS1256_ERROR_TIMEOUT`, or `ADS1256_ERROR_OVERRUN` (`read_stream()` with DRDY pin skipped a conversion).

## Troubleshooting

- **Permission denied on /dev/spidev0.0**: `sudo usermod -a -G spi $USER`, log in again.
- **Permission denied on /dev/gpiochipN**: see the udev rule above.
- **Timeouts**: check wiring and power. With DRDY, check the chip and line number. `timeout_ms` is added to 2 conversion periods, so it doesn't need to grow with slow data rates.
- **Wrong readings**: check `v_ref` against your board's reference and the gain against the signal range. Enable the input buffer for high-impedance sources.

## Version History

### Version 4.2 (2026-10-06)
- `ads1256_scan()` reads each result before selecting the next input: the datasheet's input cycling returned the next input's data after a Linux delay (3.8 % of reads at 30 kSPS, 0.13 % at 1000 SPS on the Orange Pi 5); scans are 2-20 % slower
- `ads1256_open()` retries RESET: after a process was killed during a stream, RESET could hit the data update and get lost (5 % at 30 kSPS)
- `make hwtest`: scan input check, open after kill also at 30 kSPS

### Version 4.1 (2026-10-06)
- `ads1256_open()` detects a missing ADC (MUX read back) instead of timing out on the first read
- D0/CLKOUT is turned off in `ads1256_open()` (datasheet recommendation); write ADCON bits 6-5 to use it
- Streaming with DRDY sends SDATAC with the last sample instead of waiting one more conversion period (400 ms at 2.5 SPS); a one-sample stream uses plain RDATA
- Hardware note: pull-down on SCLK, pull-up on CS

### Version 4.0 (2026-10-04)
- **New API**: device handle `ads1256_t` and `ads1256_open()` / `ads1256_close()` instead of fd lookup in a global table of 8 slots
- **Optional DRDY pin** via kernel GPIO uAPI: RDATAC streaming, exact calibration waits
- **Any inputs** (`ads1256_set_input()`, AINCOM for single-ended) and `ads1256_scan()` with datasheet input cycling
- **Raw codes** with `ads1256_to_volts()`; `ads1256_read_stream()` replaces the duration-based `ads1256_sample()`
- Setters recalibrate automatically; the library no longer prints anything (no verbose mode, no register dump)
- No dependency on lib_spi: own spidev access with t6 and t10 timing

### Version 3.6 (2026-10-04)
- Removed operating and conversion modes (ADS1220 features that corrupted the MUX register)
- SYNC+WAKEUP before reading, t6 timing for RDATA/RREG, no STATUS polling during reset and calibration
- VREF limits per datasheet, hardware-free test, lib_spi V3.0

### Version 3.5 (2025-10-02)
- Monotonic clock for timeouts, adaptive polling, named constants, better error handling

### Version 3.4 (2025-10-01)
- Context lookup by fd, `ads1256_cleanup()`

### Version 3.3 (2025-03-18)
- Static library, verbose mode, calibration timing

## License

MIT, see the LICENSE file.
