/*
 *  ADS1256 Example - 24-bit, low-noise ADC with 8 inputs
 *
 *  Usage: ./ads1256                     DRDY on GPIO1_A3 (/dev/gpiochip1, line 3)
 *         ./ads1256 /dev/gpiochip1 22   DRDY on GPIO chip 1, line 22 (find yours: sudo gpioinfo)
 *
 *  Wiring: see ads1256_lib.h
 *  A system calibration saved by ads1256_cal is applied if it matches the settings.
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ads1256_lib.h"

#define STREAM_SAMPLES 50

/** Calibration file written by ads1256_cal (key=value lines, # comments); 0 = ok */
static int load_calibration(const char *path, ads1256_calibration_t *cal)
{
    FILE *f = fopen(path, "r");
    char line[128], key[32];
    double value;
    int found = 0;

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %31[a-z_] = %lf", key, &value) != 2) {
            continue;                      /* Comment or empty line */
        }
        if (!strcmp(key, "gain")) {
            cal->gain = (ads1256_gain_t)value, found |= 1;
        } else if (!strcmp(key, "drate")) {
            for (int d = ADS1256_DRATE_30000; d <= ADS1256_DRATE_2_5; d++) {
                if (ads1256_sps((ads1256_drate_t)d) == (float)value) {
                    cal->drate = (ads1256_drate_t)d, found |= 2;
                }
            }
        } else if (!strcmp(key, "buffer")) {
            cal->buffer = value != 0, found |= 4;
        } else if (!strcmp(key, "ofc")) {
            cal->ofc = (int32_t)value, found |= 8;
        } else if (!strcmp(key, "fsc")) {
            cal->fsc = (uint32_t)value, found |= 16;
        } else if (!strcmp(key, "full_scale")) {
            cal->full_scale = value, found |= 32;
        }
    }
    fclose(f);
    return found == 63 ? 0 : -1;
}

int main(int argc, char *argv[])
{
    char *end = NULL;
    unsigned long line = argc == 3 ? strtoul(argv[2], &end, 10) : 3;  /* GPIO1_A3 */
    if ((argc != 1 && argc != 3) ||
        (argc == 3 && (!isdigit((unsigned char)argv[2][0]) || *end || line > UINT_MAX))) {
        fprintf(stderr, "Usage: %s [gpiochip drdy_line]\n", argv[0]);
        return EXIT_FAILURE;
    }

    ads1256_config_t cfg = {
        .spi_device = "/dev/spidev4.1",
        .spi_speed_hz = 1000000,           /* 1 MHz, max is fCLKIN/4 = 1.92 MHz */
        .drdy_chip = argc == 3 ? argv[1] : "/dev/gpiochip1",
        .drdy_line = (unsigned int)line,
        .v_ref = 2.5,
        .drate = ADS1256_DRATE_1000,
        .gain = ADS1256_GAIN_1,
        .pos = ADS1256_AIN0, .neg = ADS1256_AIN1,
        .buffer = false,
        .timeout_ms = 1000,
    };
    ads1256_t adc;
    int32_t raw;

    int result = ads1256_open(&adc, &cfg);
    if (result != ADS1256_OK) {
        fprintf(stderr, "Cannot open ADS1256: %s\n", ads1256_strerror(result));
        return EXIT_FAILURE;
    }
    printf("ADS1256 at %.0f SPS, DRDY %s\n", ads1256_sps(cfg.drate),
           cfg.drdy_chip ? "on GPIO" : "polled");

    /* Saved system calibration (ads1256_cal); after the setters, which would self-calibrate */
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[512];
    ads1256_calibration_t cal;
    snprintf(path, sizeof(path), "%s%s/ads1256/calibration.conf", xdg && *xdg ? xdg : home ? home : "",
             xdg && *xdg ? "" : "/.config");
    if (load_calibration(path, &cal) == 0) {
        printf("Calibration %s: %s\n", path, ads1256_set_calibration(&adc, &cal) == ADS1256_OK ?
               "applied" : "for other settings, not used");
    }

    /* Register dump */
    printf("\nReg  Binary    Hex\n");
    for (uint8_t reg = ADS1256_REG_STATUS; reg <= ADS1256_REG_FSC2; reg++) {
        uint8_t value;
        if (ads1256_read_register(&adc, reg, &value) == ADS1256_OK) {
            printf("0x%02X ", reg);
            for (int bit = 7; bit >= 0; bit--) {
                putchar(value >> bit & 1 ? '1' : '0');
            }
            printf("  0x%02X\n", value);
        }
    }

    /* Single differential reading AIN0 - AIN1 */
    if ((result = ads1256_read(&adc, &raw)) != ADS1256_OK) {
        goto error;
    }
    printf("\nAIN0-AIN1: %.6f V\n", ads1256_to_volts(&adc, raw));

    /* All 8 inputs single-ended against AINCOM */
    uint8_t inputs[8][2];
    int32_t values[8];
    for (int i = 0; i < 8; i++) {
        inputs[i][0] = (uint8_t)(ADS1256_AIN0 + i);
        inputs[i][1] = ADS1256_AINCOM;
    }
    if ((result = ads1256_scan(&adc, inputs, 8, values)) != ADS1256_OK) {
        goto error;
    }
    printf("\n");
    for (int i = 0; i < 8; i++) {
        printf("AIN%d-COM: %.6f V\n", i, ads1256_to_volts(&adc, values[i]));
    }

    /* Consecutive conversions of AIN0 - AIN1 */
    int32_t samples[STREAM_SAMPLES];
    size_t count = 0;
    if ((result = ads1256_set_input(&adc, ADS1256_AIN0, ADS1256_AIN1)) != ADS1256_OK) {
        goto error;
    }
    result = ads1256_read_stream(&adc, samples, STREAM_SAMPLES, &count);
    if (result == ADS1256_ERROR_OVERRUN && count > 0) {
        /* Linux isn't real-time: at high data rates a conversion can be missed, keep the valid ones */
        printf("\nStream overran after %zu samples (see README, measured streaming limits)\n", count);
    } else if (result != ADS1256_OK) {
        fprintf(stderr, "Stream stopped after %zu samples\n", count);
        goto error;
    }
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += ads1256_to_volts(&adc, samples[i]);
    }
    printf("\nAIN0-AIN1 average of %zu samples: %.6f V\n", count, sum / (double)count);

    ads1256_close(&adc);
    return EXIT_SUCCESS;

error:
    fprintf(stderr, "ADS1256 error: %s\n", ads1256_strerror(result));
    ads1256_close(&adc);
    return EXIT_FAILURE;
}
