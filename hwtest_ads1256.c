/*
 *  Hardware self-check: needs a connected ADS1256 with DRDY wired. Inputs may float,
 *  no check looks at measured values. Covers what the emulator in test_ads1256.c can't:
 *   1. A stream ends with SDATAC in its last read: the chip takes WREG/RREG right
 *      after, and the stream doesn't wait one more conversion period
 *   2. ads1256_open() works after a process was killed in the middle of a stream
 *   3. Without the DRDY pin, fixed calibration waits (datasheet + 10 %) are long enough
 *      (the test watches DRDY itself, the library polls)
 *   4. ads1256_scan() returns each input's own conversion: alternating pairs
 *      (AIN0, AINCOM) / (AINCOM, AIN0) read +V / -V, the neighbour's data has the wrong sign
 *
 *  Usage: ./hwtest_ads1256 [gpiochip drdy_line]   (same wiring as ads1256_example)
 *  Build & run: make hwtest
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/gpio.h>
#include "ads1256_lib.h"

#define KILLS_PER_RATE 20
#define SCAN_SECONDS 15

static ads1256_config_t base = {
    .spi_device = "/dev/spidev4.1",
    .spi_speed_hz = 1000000,
    .drdy_chip = "/dev/gpiochip1",
    .drdy_line = 3,                                    /* GPIO1_A3 */
    .v_ref = 2.5,
    .drate = ADS1256_DRATE_500,
    .gain = ADS1256_GAIN_1,
    .pos = ADS1256_AIN0, .neg = ADS1256_AIN1,
    .buffer = false,
    .timeout_ms = 1000,
};
static int failures;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s  ", ok ? "PASS" : "FAIL");
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    failures += !ok;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static double period_ms(ads1256_drate_t drate)
{
    return 1000.0 / ads1256_sps(drate);
}

static bool open_adc(ads1256_t *adc, const ads1256_config_t *cfg)
{
    int result = ads1256_open(adc, cfg);
    if (result != ADS1256_OK) {
        check(false, "ads1256_open: %s", ads1256_strerror(result));
    }
    return result == ADS1256_OK;
}


/* ===== 1. Stream stop ===== */

static void test_stream_stop(void)
{
    ads1256_t adc;
    int32_t raw[10];
    size_t count;
    uint8_t reg;

    if (!open_adc(&adc, &base)) {
        return;
    }
    check(ads1256_read_register(&adc, ADS1256_REG_ADCON, &reg) == ADS1256_OK && reg == 0x00,
          "open: ADCON = 0x%02X (CLKOUT off, PGA 1)", reg);

    /* After each stream the chip must be out of RDATAC: a new MUX value must be written and read back */
    int streams = 200, clean = 0, mismatches = 0, errors = 0;
    for (int i = 0; i < streams; i++) {
        int result = ads1256_read_stream(&adc, raw, 5, &count);
        clean += result == ADS1256_OK;
        errors += result != ADS1256_OK && result != ADS1256_ERROR_OVERRUN;
        uint8_t pos = (uint8_t)(i % 8), neg = (uint8_t)((pos + 1 + i / 8 % 8) % 9);  /* Never pos */
        if (ads1256_set_input(&adc, pos, neg) != ADS1256_OK ||
            ads1256_read_register(&adc, ADS1256_REG_MUX, &reg) != ADS1256_OK ||
            reg != (uint8_t)(pos << 4 | neg)) {
            mismatches++;
        }
    }
    check(mismatches == 0 && errors == 0 && clean >= streams * 9 / 10,
          "500 SPS: %d streams of 5, %d clean (others overrun), %d MUX write/read-back mismatches, %d errors",
          streams, clean, mismatches, errors);

    /* First result t18 = ~1 period after SYNC, then one per period: n samples take ~n periods.
     * Stopping with a separate SDATAC after a fresh DRDY took one period more. */
    static const ads1256_drate_t rates[] = { ADS1256_DRATE_100, ADS1256_DRATE_10 };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        if (ads1256_set_drate(&adc, rates[r]) != ADS1256_OK) {
            check(false, "set_drate %.0f SPS", ads1256_sps(rates[r]));
            continue;
        }
        for (size_t n = 2; n <= 10; n += 8) {
            int result, tries = 0;
            double periods;
            do {  /* Overruns come every ~10 s at 100 SPS (README); this checks only the stop */
                uint64_t t0 = now_ns();
                result = ads1256_read_stream(&adc, raw, n, &count);
                periods = (double)(now_ns() - t0) / 1e6 / period_ms(rates[r]);
            } while (result == ADS1256_ERROR_OVERRUN && ++tries < 3);
            check(result == ADS1256_OK && count == n && periods < (double)n + 0.5,
                  "%.0f SPS: stream of %zu took %.2f periods (limit %.1f): %s",
                  ads1256_sps(rates[r]), n, periods, (double)n + 0.5, ads1256_strerror(result));
        }
    }
    ads1256_close(&adc);
}


/* ===== 2. Open after a killed stream ===== */

static void stream_forever(const ads1256_config_t *cfg, int ready_fd)
{
    static int32_t raw[100000];
    ads1256_t adc;
    if (ads1256_open(&adc, cfg) != ADS1256_OK || write(ready_fd, "x", 1) != 1) {
        _exit(EXIT_FAILURE);
    }
    for (;;) {
        ads1256_read_stream(&adc, raw, sizeof(raw) / sizeof(raw[0]), NULL);
    }
}

static void test_open_after_kill(void)
{
    /* 30 kSPS: a RESET hitting the data update is lost most often there (~5 % per try) */
    static const ads1256_drate_t rates[] = { ADS1256_DRATE_30000, ADS1256_DRATE_500, ADS1256_DRATE_10 };

    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        ads1256_config_t cfg = base;
        cfg.drate = rates[r];
        int failed = 0, first_error = ADS1256_OK;

        for (int i = 0; i < KILLS_PER_RATE; i++) {
            int ready[2];
            char c;
            if (pipe(ready) < 0) {
                check(false, "pipe");
                return;
            }
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                close(ready[0]);
                stream_forever(&cfg, ready[1]);
            }
            close(ready[1]);
            bool started = pid > 0 && read(ready[0], &c, 1) == 1;
            close(ready[0]);
            if (started) {
                usleep((useconds_t)(5000 + rand() % 300000));  /* Kill at a random point of the stream */
                kill(pid, SIGKILL);
            }
            if (pid > 0) {
                waitpid(pid, NULL, 0);
            }
            if (!started) {
                check(false, "child could not open the ADC");
                return;
            }

            ads1256_t adc;
            int32_t raw;
            int result = ads1256_open(&adc, &cfg);
            if (result == ADS1256_OK) {
                result = ads1256_read(&adc, &raw);
                ads1256_close(&adc);
            }
            if (result != ADS1256_OK) {
                failed++;
                first_error = first_error != ADS1256_OK ? first_error : result;
            }
        }
        check(failed == 0, "%.0f SPS: open + read after SIGKILL during stream, %d of %d failed%s%s",
              ads1256_sps(rates[r]), failed, KILLS_PER_RATE,
              failed ? ", first: " : "", failed ? ads1256_strerror(first_error) : "");
    }
}


/* ===== 3. Calibration waits without DRDY pin ===== */

/** DRDY line with falling edge events (kernel timestamps, CLOCK_MONOTONIC), non-blocking */
static int open_drdy_events(void)
{
    int chip = open(base.drdy_chip, O_RDONLY | O_CLOEXEC);
    if (chip < 0) {
        return -1;
    }
    struct gpio_v2_line_request req = {
        .offsets = { base.drdy_line },
        .consumer = "ads1256-hwtest",
        .config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING,
        .num_lines = 1,
    };
    int result = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip);
    if (result < 0 || fcntl(req.fd, F_SETFL, O_NONBLOCK) < 0) {
        return -1;
    }
    return req.fd;
}

/** First queued falling edge at or after `after`, 0 if none; consumes the queue */
static uint64_t first_edge(int fd, uint64_t after)
{
    struct gpio_v2_line_event events[64];
    uint64_t first = 0;
    ssize_t len;
    while ((len = read(fd, events, sizeof(events))) > 0) {
        for (size_t i = 0; i < (size_t)len / sizeof(events[0]); i++) {
            if (!first && events[i].timestamp_ns >= after) {
                first = events[i].timestamp_ns;
            }
        }
    }
    return first;
}

/**
 * The calibration end is the first DRDY falling edge after 30 % of the library's wait
 * (by then the command was sent and earlier conversions were cut off). An edge before
 * the wait ended is margin; an edge only after it means the library sent commands too early.
 */
static void test_calibration_waits(void)
{
    static const uint8_t cmds[] = { ADS1256_CMD_SELFCAL, ADS1256_CMD_SELFOCAL, ADS1256_CMD_SELFGCAL };
    static const char *names[] = { "SELFCAL", "SELFOCAL", "SELFGCAL" };
    ads1256_config_t cfg = base;
    cfg.drdy_chip = NULL;                              /* Library polls, the test watches DRDY */
    ads1256_t adc;

    if (!open_adc(&adc, &cfg)) {
        return;
    }
    int fd = open_drdy_events();
    if (fd < 0) {
        check(false, "cannot open DRDY line %s:%u", base.drdy_chip, base.drdy_line);
        ads1256_close(&adc);
        return;
    }

    double worst = 100.0;
    const char *worst_at = "";
    char where[32];
    int late = 0, errors = 0;
    printf("      Margin of calibration waits [%% of wait]\n      SPS      SELFCAL  SELFOCAL  SELFGCAL\n");
    for (int d = ADS1256_DRATE_30000; d <= ADS1256_DRATE_2_5; d++) {
        if (ads1256_set_drate(&adc, (ads1256_drate_t)d) != ADS1256_OK) {
            errors++;
            continue;
        }
        printf("      %-7g", ads1256_sps((ads1256_drate_t)d));
        for (size_t c = 0; c < sizeof(cmds); c++) {
            first_edge(fd, UINT64_MAX);                /* Drop old edges */
            uint64_t t0 = now_ns();
            int result = ads1256_calibrate(&adc, cmds[c]);
            uint64_t t1 = now_ns();
            uint64_t end = first_edge(fd, t0 + (t1 - t0) * 3 / 10);
            if (!end) {                                /* Still calibrating when the library went on */
                struct pollfd pfd = { .fd = fd, .events = POLLIN };
                poll(&pfd, 1, 3000);
                end = first_edge(fd, t1);
            }
            if (result != ADS1256_OK || !end) {
                errors++;
                printf("   error  ");
                continue;
            }
            double margin = ((double)t1 - (double)end) * 100.0 / (double)(t1 - t0);
            late += margin < 0;
            if (margin < worst) {
                worst = margin;
                snprintf(where, sizeof(where), "%s at %g SPS", names[c], ads1256_sps((ads1256_drate_t)d));
                worst_at = where;
            }
            printf("  %6.1f%s", margin, margin < 0 ? " !" : "  ");
        }
        putchar('\n');
    }
    check(late == 0 && errors == 0, "calibration waits without DRDY: %d too short, %d errors, worst margin %.1f %% (%s)",
          late, errors, worst, worst_at);
    close(fd);
    ads1256_close(&adc);
}


/* ===== 4. Scan keeps inputs apart ===== */

static void test_scan_inputs(void)
{
    static const ads1256_drate_t rates[] = { ADS1256_DRATE_3750, ADS1256_DRATE_1000 };
    ads1256_config_t cfg = base;
    cfg.pos = ADS1256_AIN0;
    cfg.neg = ADS1256_AINCOM;
    ads1256_t adc;
    int32_t raw[8];

    if (!open_adc(&adc, &cfg)) {
        return;
    }
    /* Floating AIN0 reads about 1.2 V on the tested module; near 0 the sign tells nothing */
    if (ads1256_read(&adc, &raw[0]) != ADS1256_OK || fabs(ads1256_to_volts(&adc, raw[0])) < 0.1) {
        printf("SKIP  AIN0-AINCOM = %.3f V, needs at least 0.1 V (leave AIN0 floating or connect a voltage)\n",
               ads1256_to_volts(&adc, raw[0]));
        ads1256_close(&adc);
        return;
    }
    uint8_t inputs[8][2];
    for (int i = 0; i < 8; i++) {
        inputs[i][0] = i % 2 ? ADS1256_AINCOM : ADS1256_AIN0;
        inputs[i][1] = i % 2 ? ADS1256_AIN0 : ADS1256_AINCOM;
    }
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        size_t reads = 0, wrong = 0;
        int result = ads1256_set_drate(&adc, rates[r]);
        uint64_t end = now_ns() + SCAN_SECONDS * 1000000000ull;
        while (result == ADS1256_OK && now_ns() < end) {
            if ((result = ads1256_scan(&adc, inputs, 8, raw)) == ADS1256_OK) {
                int sign = raw[0] < 0 ? -1 : 1;        /* AIN0-AINCOM may be negative too */
                for (int i = 0; i < 8; i++) {
                    wrong += (i % 2 ? -sign : sign) * raw[i] < 0;
                }
                reads += 8;
            }
        }
        check(result == ADS1256_OK && wrong == 0, "%.0f SPS: %zu scanned reads, %zu with the neighbour's sign: %s",
              ads1256_sps(rates[r]), reads, wrong, ads1256_strerror(result));
    }
    ads1256_close(&adc);
}


int main(int argc, char *argv[])
{
    char *end = NULL;
    unsigned long line = argc == 3 ? strtoul(argv[2], &end, 10) : base.drdy_line;
    if ((argc != 1 && argc != 3) ||
        (argc == 3 && (!isdigit((unsigned char)argv[2][0]) || *end || line > UINT_MAX))) {
        fprintf(stderr, "Usage: %s [gpiochip drdy_line]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 3) {
        base.drdy_chip = argv[1];
        base.drdy_line = (unsigned int)line;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand((unsigned int)now_ns());

    printf("1. Stream stop (SDATAC in the last read)\n");
    test_stream_stop();
    printf("\n2. Open after a process was killed during a stream (%d kills per rate)\n", KILLS_PER_RATE);
    test_open_after_kill();
    printf("\n3. Calibration waits without DRDY pin\n");
    test_calibration_waits();
    printf("\n4. Scan keeps inputs apart (%d s per rate)\n", SCAN_SECONDS);
    test_scan_inputs();

    printf("\n%s: %d failed\n", failures ? "FAILED" : "All hardware checks passed", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
