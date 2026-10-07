/*
 *  ADS1256 system calibration: zero (SYSOCAL) and optionally full scale (SYSGCAL) with the
 *  signals applied at the inputs, saved as text for ads1256_set_calibration()
 *  (ads1256_apply_calibration() loads the one for the current settings).
 *
 *  Usage: ads1256_cal [-s spidev] [-d gpiochip:line] [-p pos] [-n neg] [-g gain] [-r sps]
 *                     [-b] [-v vref] [-V volts] [-o file]
 *    -p, -n    inputs 0-7, 8 = AINCOM (default 0 and 1)
 *    -V volts  also calibrate gain with this voltage applied (full scale = 2 * vref / gain):
 *              80-100 % of full scale runs SYSGCAL, 20-80 % scales from a measurement
 *              (e.g. a 2.5 V reference at gain 1)
 *  Refused (nothing saved): offset over 1 % of full scale, or the -V voltage reading 10 % off.
 *    -o file   default $XDG_CONFIG_HOME/ads1256/cal-g<gain>-<sps>sps-buf<0|1>.conf
 *              (~/.config/ads1256/...), one file per setting: run once for each gain, rate, buffer
 *  Use the settings of the measuring program: the file is refused for other gain, rate, buffer.
 */

#define _POSIX_C_SOURCE 200809L   /* getopt, mkdir, O_DIRECTORY */

#include <errno.h>
#include <fcntl.h>
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

/** Option value: the whole string a number (e.g. not "2,5"), else exit naming the option */
static double num_arg(int opt, const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno) {
        fprintf(stderr, "-%c: '%s' is not a number\n", opt, s);
        exit(EXIT_FAILURE);
    }
    return v;
}

/** Option value: the whole string an integer in [min, max], else exit naming the option */
static long int_arg(int opt, const char *s, long min, long max)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < min || v > max) {
        fprintf(stderr, "-%c: '%s' is not an integer %ld-%ld\n", opt, s, min, max);
        exit(EXIT_FAILURE);
    }
    return v;
}

/** Create the directories of a file path (mkdir -p); 0 = ok, -1 with errno */
static int make_dirs(char *path)
{
    for (char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        int failed = mkdir(path, 0755) < 0 && errno != EEXIST;
        *p = '/';
        if (failed) {
            return -1;
        }
    }
    return 0;
}

/** fsync the directory of a file path, so a rename in it survives a power loss; 0 = ok */
static int sync_dir(const char *path)
{
    char dir[512];
    const char *slash = strrchr(path, '/');
    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) + (slash == path) : 1, slash ? path : ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    int failed = fd < 0 || fsync(fd) != 0;
    if (fd >= 0) {
        close(fd);
    }
    return failed ? -1 : 0;
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
            cfg.drdy_line = (unsigned int)int_arg(opt, colon + 1, 0, 65535);
            break;
        case 'p': cfg.pos = (uint8_t)int_arg(opt, optarg, 0, ADS1256_AINCOM); break;
        case 'n': cfg.neg = (uint8_t)int_arg(opt, optarg, 0, ADS1256_AINCOM); break;
        case 'g': cfg.gain = (ads1256_gain_t)int_arg(opt, optarg, 1, 64); break;
        case 'r': {
            const float sps = (float)num_arg(opt, optarg);
            cfg.drate = (ads1256_drate_t)16;               /* Invalid unless found */
            for (int d = ADS1256_DRATE_30000; d <= ADS1256_DRATE_2_5; d++) {
                if (ads1256_sps((ads1256_drate_t)d) == sps) {
                    cfg.drate = (ads1256_drate_t)d;
                }
            }
            if (cfg.drate == 16) {
                fprintf(stderr, "-r: no data rate %s SPS (30000, 15000, ... 5, 2.5)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        }
        case 'b': cfg.buffer = true; break;
        case 'v': cfg.v_ref = num_arg(opt, optarg); break;
        case 'V': v_cal = num_arg(opt, optarg); break;
        case 'o':
            if ((size_t)snprintf(path, sizeof(path), "%s", optarg) >= sizeof(path)) {
                fprintf(stderr, "-o: path too long\n");
                return EXIT_FAILURE;
            }
            break;
        default:
            fprintf(stderr, "Usage: %s [-s spidev] [-d gpiochip:line] [-p pos] [-n neg] [-g gain] [-r sps]"
                    " [-b] [-v vref] [-V volts] [-o file]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    ads1256_t adc;
    int result = ads1256_open(&adc, &cfg);              /* Also checks the settings */
    if (result != ADS1256_OK) {
        fprintf(stderr, "Cannot open ADS1256: %s\n", ads1256_strerror(result));
        return EXIT_FAILURE;
    }
    const double full = 2.0 * cfg.v_ref / cfg.gain;    /* Nominal full scale before calibration */
    const bool sysgcal = v_cal >= 0.8 * full;         /* Below: gain from a measurement */
    if (v_cal && !(v_cal >= 0.2 * full && v_cal <= full)) {
        fprintf(stderr, "-V must be 20-100 %% of the full scale %.4g V\n", full);
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    if (!*path) {                                       /* After open: it checked the settings */
        if (ads1256_calibration_path(&cfg, path, sizeof(path)) != ADS1256_OK) {
            fprintf(stderr, "No default file (%s), use -o file\n", strerror(errno));
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
        if (make_dirs(path) != 0) {
            fprintf(stderr, "Cannot create the directory of %s: %s, use -o file\n", path, strerror(errno));
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
    }
    printf("%s-%s, gain %d, %g SPS, buffer %s, full scale %.6g V\n", input_name(cfg.pos), input_name(cfg.neg),
           cfg.gain, ads1256_sps(cfg.drate), cfg.buffer ? "on" : "off", full);

    ads1256_calibration_t cal;
    double volts;
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "Connect %s and %s together (0 V, at the sensor if possible)",
             input_name(cfg.pos), input_name(cfg.neg));
    /* Measure before calibrating: afterwards the input reads as expected whatever it was */
    if (wait_enter(prompt) != 0 || (result = average_volts(&adc, &volts)) != ADS1256_OK) {
        goto error;
    }
    printf("Offset %+.7f V\n", volts);
    if (volts > 0.01 * full || volts < -0.01 * full) {
        fprintf(stderr, "Offset over 1 %% of the full scale, are the inputs connected together? Not saved\n");
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    if ((result = ads1256_calibrate(&adc, ADS1256_CMD_SYSOCAL)) != ADS1256_OK) {
        goto error;
    }

    if (v_cal) {
        snprintf(prompt, sizeof(prompt), "Apply %.6g V (+ at %s)", v_cal, input_name(cfg.pos));
        if (wait_enter(prompt) != 0 || (result = average_volts(&adc, &volts)) != ADS1256_OK) {
            goto error;
        }
        if (!(volts > 0.9 * v_cal && volts < 1.1 * v_cal)) {
            fprintf(stderr, "Reads %.6f V instead of about %.6g V, check the wiring; not saved\n", volts, v_cal);
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
        if (sysgcal && (result = ads1256_calibrate(&adc, ADS1256_CMD_SYSGCAL)) != ADS1256_OK) {
            goto error;
        }
    }
    if ((result = ads1256_get_calibration(&adc, &cal)) != ADS1256_OK) {
        goto error;
    }
    cal.full_scale = 0;                                /* Offset only: the program's v_ref stays */
    if (v_cal) {
        /* SYSGCAL: the applied voltage now reads as code 2^23; else self-calibrated FSC stays, the scale follows V */
        cal.full_scale = sysgcal ? v_cal : full * v_cal / volts;
        if ((result = ads1256_set_calibration(&adc, &cal)) != ADS1256_OK ||
            (result = average_volts(&adc, &volts)) != ADS1256_OK) {
            goto error;
        }
        printf("Gain calibrated, reads %.7f V (applied %.6g V)\n", volts, v_cal);
        if (volts < 0.99 * v_cal || volts > 1.01 * v_cal) {  /* SYSGCAL out of its range, or the input changed */
            fprintf(stderr, "Calibration did not take (more than 1 %% off), not saved\n");
            ads1256_close(&adc);
            return EXIT_FAILURE;
        }
    }

    char date[32];
    time_t now = time(NULL);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&now));
    /* Write a temporary file and rename it: a crash leaves the old file or the new, never half */
    char tmp[sizeof(path) + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        perror(tmp);
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    fprintf(f, "# ADS1256 system calibration %s, %s-%s, ", date, input_name(cfg.pos), input_name(cfg.neg));
    if (v_cal) {
        fprintf(f, "offset and gain (%s) at %.6g V\n", sysgcal ? "SYSGCAL" : "measured", v_cal);
    } else {
        fprintf(f, "offset only\n");
    }
    fprintf(f, "# Load with ads1256_apply_calibration(), valid only for these settings\n");
    fprintf(f, "gain=%d\ndrate=%g\nbuffer=%d\nofc=%ld\nfsc=%lu\nfull_scale=%.9g\n", cal.gain,
            ads1256_sps(cal.drate), cal.buffer, (long)cal.ofc, (unsigned long)cal.fsc, cal.full_scale);
    int failed = fflush(f) != 0 || ferror(f) || fsync(fileno(f)) != 0;  /* ferror: a failed fprintf */
    if (fclose(f) != 0 || failed || rename(tmp, path) != 0) {
        perror(tmp);
        remove(tmp);
        ads1256_close(&adc);
        return EXIT_FAILURE;
    }
    if (sync_dir(path) != 0) {
        fprintf(stderr, "Saved to %s, but not synced to disk: %s\n", path, strerror(errno));
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
