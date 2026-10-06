/*
 *  ADS1256 system calibration: zero (SYSOCAL) and optionally full scale (SYSGCAL) with the
 *  signals applied at the inputs, saved as text for ads1256_set_calibration()
 *  (load_calibration() in ads1256_example.c reads it).
 *
 *  Usage: ads1256_cal [-s spidev] [-d gpiochip:line] [-p pos] [-n neg] [-g gain] [-r sps]
 *                     [-b] [-v vref] [-V volts] [-o file]
 *    -p, -n    inputs 0-7, 8 = AINCOM (default 0 and 1)
 *    -V volts  also calibrate gain with this voltage applied (full scale = 2 * vref / gain):
 *              80-100 % of full scale runs SYSGCAL, 20-80 % scales from a measurement
 *              (e.g. a 2.5 V reference at gain 1)
 *    -o file   default $XDG_CONFIG_HOME/ads1256/calibration.conf (~/.config/ads1256/...)
 *  Use the settings of the measuring program: the file is refused for other gain, rate, buffer.
 */

#define _POSIX_C_SOURCE 200809L   /* getopt, mkdir */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ads1256_lib.h"

#define VERIFY_READS 10

static const char *input_name(uint8_t in)
{
    static const char *names[] = { "AIN0", "AIN1", "AIN2", "AIN3", "AIN4", "AIN5", "AIN6", "AIN7", "AINCOM" };
    return in <= ADS1256_AINCOM ? names[in] : "?";
}

/** $XDG_CONFIG_HOME/ads1256/calibration.conf, creating the directories; 0 = ok */
static int default_path(char *path, size_t size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char dir[512];
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s", xdg);
    } else if (home) {
        snprintf(dir, sizeof(dir), "%s/.config", home);
    } else {
        return -1;
    }
    if ((mkdir(dir, 0755) < 0 && errno != EEXIST) ||
        (size_t)snprintf(path, size, "%s/ads1256", dir) >= size ||
        (mkdir(path, 0755) < 0 && errno != EEXIST)) {
        return -1;
    }
    return (size_t)snprintf(path, size, "%s/ads1256/calibration.conf", dir) >= size ? -1 : 0;
}

static int wait_enter(const char *prompt)
{
    int c;
    printf("%s, then press Enter ", prompt);
    fflush(stdout);
    while ((c = getchar()) != '\n' && c != EOF) {
    }
    return c == EOF ? -1 : 0;
}

/** Average of VERIFY_READS readings in volts */
static int average_volts(ads1256_t *adc, double *volts)
{
    double sum = 0;
    for (int i = 0; i < VERIFY_READS; i++) {
        int32_t raw;
        int result = ads1256_read(adc, &raw);
        if (result != ADS1256_OK) {
            return result;
        }
        sum += ads1256_to_volts(adc, raw);
    }
    *volts = sum / VERIFY_READS;
    return ADS1256_OK;
}

int main(int argc, char *argv[])
{
    ads1256_config_t cfg = {
        .spi_device = "/dev/spidev4.1", .spi_speed_hz = 1000000,
        .v_ref = 2.5, .drate = ADS1256_DRATE_2_5, .gain = ADS1256_GAIN_1,
        .pos = ADS1256_AIN0, .neg = ADS1256_AIN1, .buffer = false, .timeout_ms = 1000,
    };
    double v_cal = 0;
    char path[512] = "", chip[256];
    int opt;

    while ((opt = getopt(argc, argv, "s:d:p:n:g:r:bv:V:o:")) != -1) {
        char *colon;
        switch (opt) {
        case 's': cfg.spi_device = optarg; break;
        case 'd':
            if (!(colon = strchr(optarg, ':')) || (size_t)(colon - optarg) >= sizeof(chip)) {
                fprintf(stderr, "-d expects gpiochip:line\n");
                return EXIT_FAILURE;
            }
            snprintf(chip, sizeof(chip), "%.*s", (int)(colon - optarg), optarg);
            cfg.drdy_chip = chip;
            cfg.drdy_line = (unsigned int)strtoul(colon + 1, NULL, 10);
            break;
        case 'p': cfg.pos = (uint8_t)atoi(optarg); break;
        case 'n': cfg.neg = (uint8_t)atoi(optarg); break;
        case 'g': cfg.gain = (ads1256_gain_t)atoi(optarg); break;
        case 'r':
            cfg.drate = (ads1256_drate_t)16;               /* Invalid unless found */
            for (int d = ADS1256_DRATE_30000; d <= ADS1256_DRATE_2_5; d++) {
                if (ads1256_sps((ads1256_drate_t)d) == (float)atof(optarg)) {
                    cfg.drate = (ads1256_drate_t)d;
                }
            }
            break;
        case 'b': cfg.buffer = true; break;
        case 'v': cfg.v_ref = atof(optarg); break;
        case 'V': v_cal = atof(optarg); break;
        case 'o': snprintf(path, sizeof(path), "%s", optarg); break;
        default:
            fprintf(stderr, "Usage: %s [-s spidev] [-d gpiochip:line] [-p pos] [-n neg] [-g gain] [-r sps]"
                    " [-b] [-v vref] [-V volts] [-o file]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (!*path && default_path(path, sizeof(path)) != 0) {
        fprintf(stderr, "Cannot create ~/.config/ads1256, use -o file\n");
        return EXIT_FAILURE;
    }
    const double full = 2.0 * cfg.v_ref / cfg.gain;    /* Nominal full scale before calibration */
    const bool sysgcal = v_cal >= 0.8 * full;         /* Below: gain from a measurement */
    if (v_cal && !(v_cal >= 0.2 * full && v_cal <= full)) {
        fprintf(stderr, "-V must be 20-100 %% of the full scale %.4g V\n", full);
        return EXIT_FAILURE;
    }

    ads1256_t adc;
    int result = ads1256_open(&adc, &cfg);
    if (result != ADS1256_OK) {
        fprintf(stderr, "Cannot open ADS1256: %s\n", ads1256_strerror(result));
        return EXIT_FAILURE;
    }
    printf("%s-%s, gain %d, %g SPS, buffer %s, full scale %.6g V\n", input_name(cfg.pos), input_name(cfg.neg),
           cfg.gain, ads1256_sps(cfg.drate), cfg.buffer ? "on" : "off", full);

    ads1256_calibration_t cal;
    double volts;
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "Connect %s and %s together (0 V, at the sensor if possible)",
             input_name(cfg.pos), input_name(cfg.neg));
    if (wait_enter(prompt) != 0 ||
        (result = ads1256_calibrate(&adc, ADS1256_CMD_SYSOCAL)) != ADS1256_OK ||
        (result = average_volts(&adc, &volts)) != ADS1256_OK) {
        goto error;
    }
    printf("Offset calibrated, reads %+.7f V\n", volts);

    if (v_cal) {
        snprintf(prompt, sizeof(prompt), "Apply %.6g V (+ at %s)", v_cal, input_name(cfg.pos));
        if (wait_enter(prompt) != 0 ||
            (result = sysgcal ? ads1256_calibrate(&adc, ADS1256_CMD_SYSGCAL)
                              : average_volts(&adc, &volts)) != ADS1256_OK) {
            goto error;
        }
    }
    if ((result = ads1256_get_calibration(&adc, &cal)) != ADS1256_OK) {
        goto error;
    }
    if (v_cal) {
        if (sysgcal) {
            cal.full_scale = v_cal;                    /* The applied voltage now reads as code 2^23 */
        } else if (volts > 0.9 * v_cal && volts < 1.1 * v_cal) {
            cal.full_scale = full * v_cal / volts;     /* Self-calibrated FSC stays, the scale follows V */
        } else {
            fprintf(stderr, "Reads %.6f V instead of about %.6g V, check the wiring; not saved\n", volts, v_cal);
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
        if ((result = ads1256_set_calibration(&adc, &cal)) != ADS1256_OK ||
            (result = average_volts(&adc, &volts)) != ADS1256_OK) {
            goto error;
        }
        printf("Gain calibrated, reads %.7f V (applied %.6g V)\n", volts, v_cal);
        if (volts < 0.99 * v_cal || volts > 1.01 * v_cal) {  /* E.g. input not at 0 V in the offset step */
            fprintf(stderr, "Calibration is inconsistent (more than 1 %% off), not saved. "
                    "Was the input at 0 V in the first step and at %.6g V in the second?\n", v_cal);
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
    }

    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&now));
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    fprintf(f, "# ADS1256 system calibration %s, %s-%s, ", date, input_name(cfg.pos), input_name(cfg.neg));
    if (v_cal) {
        fprintf(f, "offset and gain (%s) at %.6g V\n", sysgcal ? "SYSGCAL" : "measured", v_cal);
    } else {
        fprintf(f, "offset only\n");
    }
    fprintf(f, "# Load with ads1256_set_calibration(), valid only for these settings\n");
    fprintf(f, "gain=%d\ndrate=%g\nbuffer=%d\nofc=%ld\nfsc=%lu\nfull_scale=%.9g\n", cal.gain,
            ads1256_sps(cal.drate), cal.buffer, (long)cal.ofc, (unsigned long)cal.fsc, cal.full_scale);
    if (fclose(f) != 0) {
        perror(path);
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    printf("Saved to %s\n", path);
    ads1256_close(&adc);
    return EXIT_SUCCESS;

error:
    fprintf(stderr, "%s\n", result == ADS1256_OK ? "Aborted" : ads1256_strerror(result));
    ads1256_close(&adc);
    return EXIT_FAILURE;
}
