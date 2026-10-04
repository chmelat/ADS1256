# ADS1256 Library for Linux SBCs

C library for the TI ADS1256 24-bit ADC over Linux `spidev` (Orange Pi, Raspberry Pi and other single-board computers).

## Features

- Any input combination: 4 differential pairs, 8 single-ended inputs against AINCOM, or any other pair
- Data rates 2.5 SPS to 30 kSPS, PGA gain 1 to 64, optional input buffer
- Optional DRDY pin on GPIO: continuous streaming (RDATAC) and exact calibration waits; without it the library polls the STATUS register
- Fast multi-input scan using the input cycling procedure from the datasheet
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

### DRDY pin (optional)

Without DRDY, the library polls the STATUS register and waits fixed datasheet times (+10 %) after calibration, because no command may be sent before it finishes. After reset it always waits 10 ms. Each streamed sample then costs a STATUS poll plus an RDATA command, which limits throughput to roughly 1-2 kSPS.

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
- **Speed**: the Cortex-A72 is slower than the RK3588, so streaming with the DRDY pin reports `ADS1256_ERROR_OVERRUN` at a lower data rate than on the Orange Pi 5, probably below 30 kSPS.

### Orange Pi 5

- Enable SPI with a device tree overlay (`orangepi-config` or the `overlays=` line in `/boot/orangepiEnv.txt`).
- **DRDY pin**: Rockchip pin `GPIOx_yz` is `/dev/gpiochipx`, line `y * 8 + z` with A=0, B=1, C=2, D=3 (e.g. GPIO1_C6 is `/dev/gpiochip1`, line 22). Check with `sudo gpioinfo`.
- `/dev/gpiochip*` is root-only by default, see the udev rule above.

## Requirements

No libraries: only the Linux `spidev` driver and the GPIO character device (uAPI v2, kernel 5.10 or newer, used only with the DRDY pin).

## Build

```bash
make                # Example program ./ads1256
make lib            # Static library libads1256.a
make test           # Hardware-free test with an emulated ADS1256
make install        # libads1256.a to ~/lib, ads1256_lib.h to ~/include
```

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

The scan selects the next input right after DRDY and reads the previous result while the new conversion settles. This is the fastest way to measure several inputs (datasheet table 14). The last pair stays selected afterwards.

### Streaming

```c
int32_t samples[1000];
size_t count;                                /* Valid samples, also on error */
ads1256_read_stream(&adc, samples, 1000, &count);
```

With DRDY wired this uses RDATAC and returns `ADS1256_ERROR_OVERRUN` when the program can't keep up: a conversion was skipped, or a sample was read so late that the next update could overwrite it (judged by kernel timestamps of the DRDY edges). At 30 kSPS a period is 33 us while reading 24 bits at 1 MHz SCLK alone takes 24 us, so use SCLK near the 1.92 MHz maximum and expect overruns anyway. Without the pin, high data rates can't be kept up with and skipped conversions are not detected, so measure the real rate on your hardware.

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
