/*
 *  Hardware-free self-check: emulates ADS1256 behind fake ioctl (spidev) and poll.
 *  DRDY is tested both polled through STATUS and on an emulated GPIO line, whose
 *  edge events go through a real pipe (so read/O_NONBLOCK are the kernel's).
 *  Build & run: make test
 */

#define _GNU_SOURCE                                    /* ppoll */
#undef NDEBUG                                          /* The test is made of asserts */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include "ads1256_lib.h"

/* --- Emulated ADS1256 --- */
static int spi_fd = -1;
static uint8_t regs[11] = { 0x30, 0x01, 0x20, 0xF0 };  /* Reset values */
static uint8_t last_cmds[8];                           /* Last single-byte commands */
static uint32_t pending;                               /* Conversion in progress */
static uint32_t data_reg;                              /* Last finished conversion */
static int rdatac;                                     /* In Read Data Continuous mode */

/* Emulated DRDY GPIO line: conversion finishes when the library looks at the pin */
static int gpio_rd = -1, gpio_wr = -1;                 /* Pipe carrying edge events */
static int drdy_level = 1;
static int pending_valid;                              /* Conversion/calibration running */
static int edges_per_conversion = 1;                   /* 2 = a conversion was skipped */
static int commits_left = -1;                          /* >= 0: DRDY stalls after that many */
static int slow_read_us;                               /* Data read takes this long (preemption) */
static int slow_last_read;                             /* Read with SDATAC is late, SDATAC is lost */
static int duplex_stops;                               /* RDATAC ended by SDATAC during a data read */
static int stale_edge;                                 /* Add a late-delivered old event */
static int noise;                                      /* Edges keep coming, DRDY stays high */
static int fresh_edge;                                 /* An edge came since the last SPI message */
static int poll_eintr;                                 /* Next poll() is interrupted */
static int no_chip;                                    /* MISO floats high: reads are 0xFF */
static int late_rdata;                                 /* Process delayed: running conversion ends before RDATA */
static int stuck;                                      /* In RDATAC left by a killed process: WREG/RREG ignored */
static int lost_resets;                                /* While stuck, RESETs that hit the data update are lost */
static int slow_level_us;                              /* Process delayed after seeing DRDY low */
static uint64_t last_edge_ns;                          /* Timestamp of the last DRDY edge event */

static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Each input pair converts to a distinct code: MUX << 16 (AINCOM as pos gives negative) */
static int32_t code_for(uint8_t mux)
{
    int32_t v = (int32_t)mux << 16;
    return v >= 0x800000 ? v - 0x1000000 : v;
}

static void start_next_conversion(void)
{
    pending = (uint32_t)code_for(regs[ADS1256_REG_MUX]) & 0xFFFFFF;
    pending_valid = 1;
    drdy_level = 1;
}

static void finish_conversion(void)
{
    if (!pending_valid || commits_left == 0) {
        return;
    }
    struct gpio_v2_line_event old = { .timestamp_ns = 1 }, event = { .timestamp_ns = mono_ns() };
    last_edge_ns = event.timestamp_ns;
    pending_valid = 0;
    fresh_edge = 1;
    commits_left -= commits_left > 0;
    data_reg = pending;
    drdy_level = 0;
    if (stale_edge) {
        ssize_t written = write(gpio_wr, &old, sizeof(old));
        assert(written == (ssize_t)sizeof(old));
    }
    for (int i = 0; i < edges_per_conversion; i++) {
        ssize_t written = write(gpio_wr, &event, sizeof(event));
        assert(written == (ssize_t)sizeof(event));
    }
}

static void output_data(uint8_t *out)
{
    if (slow_read_us) {
        usleep((useconds_t)slow_read_us);
    }
    out[0] = (uint8_t)(data_reg >> 16); out[1] = (uint8_t)(data_reg >> 8); out[2] = (uint8_t)data_reg;
    drdy_level = 1;                                    /* DRDY goes high after 24 bits */
}

static void spi_command(const uint8_t *b, uint32_t len)  /* tx-only message: WREG or command */
{
    if (len == 3) {
        assert((b[0] & 0xF0) == ADS1256_CMD_WREG && b[1] == 0);
        if (!stuck) {
            regs[b[0] & 0x0F] = b[2];
        }
        return;
    }
    assert(len == 1);
    memmove(last_cmds + 1, last_cmds, sizeof(last_cmds) - 1);
    last_cmds[0] = b[0];
    if (b[0] == ADS1256_CMD_WAKEUP || (b[0] >= ADS1256_CMD_SELFCAL && b[0] <= ADS1256_CMD_SYSGCAL)) {
        start_next_conversion();
    } else if (b[0] == ADS1256_CMD_SYNC) {
        drdy_level = 1;
    } else if (b[0] == ADS1256_CMD_SDATAC) {
        rdatac = 0;
    } else if (b[0] == ADS1256_CMD_RESET && stuck) {
        stuck = lost_resets-- > 0;
    }
}

static void spi_message(const struct spi_ioc_transfer *t, unsigned int n)
{
    const uint8_t *first = (const uint8_t *)(unsigned long)t[0].tx_buf;
    int duplex = n == 1 && t[0].rx_buf;                /* RDATAC data, DIN carries 0, 0, 0 or SDATAC */
    if (first && first[duplex ? 2 : 0] == ADS1256_CMD_SDATAC) {
        assert(fresh_edge || commits_left == 0);       /* Right after DRDY edge, unless stalled */
    }
    fresh_edge = 0;
    assert(t[n - 1].delay_usecs >= 2);                 /* t10 = 1.04 us before CS high */
    if (duplex) {                                      /* SDATAC only last, after all 24 bits */
        assert(t[0].len == 3 && rdatac && first && first[0] == 0 && first[1] == 0);
        int sdatac = first[2] == ADS1256_CMD_SDATAC;
        if (sdatac && slow_last_read) {
            usleep(10000);                             /* Period is 10 ms: DRDY rose, SDATAC is ignored */
        }
        output_data((uint8_t *)(unsigned long)t[0].rx_buf);
        start_next_conversion();
        if (sdatac && !slow_last_read) {
            spi_command(&first[2], 1);                 /* Leaves RDATAC, logs SDATAC */
            duplex_stops++;
        }
        return;
    }
    if (n == 1 && t[0].tx_buf) {
        spi_command((const uint8_t *)(unsigned long)t[0].tx_buf, t[0].len);
        return;
    }

    assert(t[0].delay_usecs >= 7);                     /* t6 = 6.5 us */
    const uint8_t *cmd = (const uint8_t *)(unsigned long)t[0].tx_buf;
    uint8_t *out = (uint8_t *)(unsigned long)t[1].rx_buf;
    if ((cmd[0] & 0xF0) == ADS1256_CMD_RREG) {
        assert(t[0].len == 2 && t[1].len == 1);
        if ((cmd[0] & 0x0F) == ADS1256_REG_STATUS) {
            data_reg = pending;                        /* Polled DRDY: conversion done */
        }
        uint8_t reg = cmd[0] & 0x0F;
        out[0] = no_chip || stuck ? 0xFF :
                 reg == ADS1256_REG_STATUS ? regs[reg] & ~ADS1256_STATUS_DRDY_MASK : regs[reg];  /* DRDY low */
    } else if (cmd[0] == ADS1256_CMD_RDATA) {
        assert(t[0].len == 1 && t[1].len == 3);
        if (late_rdata) {
            data_reg = pending;                        /* Same input unless MUX was switched before */
        }
        output_data(out);
    } else {
        assert(cmd[0] == ADS1256_CMD_RDATAC && t[1].len == 3 && !rdatac);
        rdatac = 1;
        output_data(out);
        start_next_conversion();
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (request == SPI_IOC_WR_MODE) {
        assert(*(uint8_t *)arg == SPI_MODE_1);
        spi_fd = fd;
    } else if (request == SPI_IOC_WR_MAX_SPEED_HZ) {
        assert(fd == spi_fd && *(uint32_t *)arg == 1000000);
    } else if (request == SPI_IOC_MESSAGE(1) || request == SPI_IOC_MESSAGE(2)) {
        assert(fd == spi_fd);
        spi_message(arg, request == SPI_IOC_MESSAGE(1) ? 1 : 2);
    } else if (request == GPIO_V2_GET_LINE_IOCTL) {     /* Request DRDY line */
        struct gpio_v2_line_request *req = arg;
        int p[2];
        assert(req->num_lines == 1 && req->offsets[0] == 22);
        assert(req->config.flags == (GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING));
        int piped = pipe(p);
        assert(piped == 0);
        gpio_rd = req->fd = p[0];
        gpio_wr = p[1];
    } else {
        struct gpio_v2_line_values *values = arg;     /* Read DRDY level */
        assert(request == GPIO_V2_LINE_GET_VALUES_IOCTL && fd == gpio_rd && values->mask == 1);
        finish_conversion();
        values->bits = (uint64_t)drdy_level;
        if (slow_level_us && !drdy_level) {
            usleep((useconds_t)slow_level_us);
        }
    }
    return 0;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)  /* Wait for DRDY edge */
{
    (void)timeout;
    assert(nfds == 1);                                 /* glibc marks fds write-only: no reads */
    if (poll_eintr) {
        poll_eintr = 0;
        errno = EINTR;
        return -1;
    }
    if (noise) {
        struct gpio_v2_line_event event = { .timestamp_ns = 1 };
        ssize_t written = write(gpio_wr, &event, sizeof(event));
        assert(written == (ssize_t)sizeof(event));
        fds[0].revents = POLLIN;
        return 1;
    }
    finish_conversion();
    struct timespec now = { 0 };
    return ppoll(fds, nfds, &now, NULL);               /* Real pipe state, 0 = timeout */
}

/* Read, scan and stream must return each input's own conversion */
static void check_acquisition(ads1256_t *adc)
{
    int32_t raw;
    assert(ads1256_set_input(adc, ADS1256_AIN0, ADS1256_AIN1) == ADS1256_OK);
    assert(ads1256_read(adc, &raw) == ADS1256_OK);
    assert(raw == code_for(0x01));
    assert(ads1256_set_input(adc, ADS1256_AINCOM, ADS1256_AIN0) == ADS1256_OK);
    assert(ads1256_read(adc, &raw) == ADS1256_OK);
    assert(raw == -0x800000);                          /* Sign extension */

    uint8_t inputs[3][2] = {
        { ADS1256_AIN0, ADS1256_AINCOM }, { ADS1256_AIN6, ADS1256_AIN7 }, { ADS1256_AIN5, ADS1256_AINCOM }
    };
    int32_t values[3];
    assert(ads1256_scan(adc, inputs, 3, values) == ADS1256_OK);
    assert(values[0] == code_for(0x08) && values[1] == code_for(0x67) && values[2] == code_for(0x58));
    assert(adc->cfg.pos == ADS1256_AIN5 && adc->cfg.neg == ADS1256_AINCOM);  /* Last pair stays */
    late_rdata = 1;                                    /* A delay before RDATA must not mix up inputs */
    assert(ads1256_scan(adc, inputs, 3, values) == ADS1256_OK);
    assert(values[0] == code_for(0x08) && values[1] == code_for(0x67) && values[2] == code_for(0x58));
    assert(ads1256_read(adc, &raw) == ADS1256_OK && raw == code_for(0x58));
    late_rdata = 0;

    int32_t stream[5];
    size_t count;
    assert(ads1256_set_input(adc, ADS1256_AIN4, ADS1256_AIN5) == ADS1256_OK);
    assert(ads1256_read_stream(adc, stream, 5, &count) == ADS1256_OK && count == 5);
    for (int i = 0; i < 5; i++) {
        assert(stream[i] == code_for(0x45));
    }
    assert(!rdatac);
}

int main(void)
{
    ads1256_config_t cfg = {
        .spi_device = "/dev/null", .spi_speed_hz = 1000000,  /* SPI ioctls go to the fake */
        .v_ref = 2.5, .drate = ADS1256_DRATE_30000, .gain = ADS1256_GAIN_8,
        .pos = ADS1256_AIN3, .neg = ADS1256_AINCOM, .buffer = true, .timeout_ms = 100
    };
    ads1256_t adc;
    int32_t raw;

    /* Invalid configuration is rejected before touching the device, fds safe to close */
    ads1256_config_t bad = cfg;
    bad.v_ref = 5.0;
    assert(ads1256_open(&adc, &bad) == ADS1256_ERROR_PARAMETER);
    bad.v_ref = NAN;
    assert(ads1256_open(&adc, &bad) == ADS1256_ERROR_PARAMETER);
    assert(adc.spi_fd == -1 && adc.drdy_fd == -1);
    bad = cfg;
    bad.pos = bad.neg;
    assert(ads1256_open(&adc, &bad) == ADS1256_ERROR_PARAMETER);
    assert(ads1256_open(&adc, NULL) == ADS1256_ERROR_PARAMETER);

    /* No chip on the bus: open fails at once instead of a timeout on the first read */
    no_chip = 1;
    assert(ads1256_open(&adc, &cfg) == ADS1256_ERROR_COMMUNICATION);
    assert(adc.spi_fd == -1 && adc.drdy_fd == -1);
    no_chip = 0;

    /* Chip left in RDATAC: a RESET lost in the data update is retried, but not forever */
    stuck = 1;
    lost_resets = 2;
    assert(ads1256_open(&adc, &cfg) == ADS1256_OK && !stuck);
    ads1256_close(&adc);
    stuck = 1;
    lost_resets = 3;
    assert(ads1256_open(&adc, &cfg) == ADS1256_ERROR_COMMUNICATION && stuck);
    stuck = 0;

    /* ===== DRDY polled through STATUS ===== */

    /* Open writes all settings and self-calibrates */
    assert(ads1256_open(&adc, &cfg) == ADS1256_OK);
    assert(adc.spi_fd == spi_fd && adc.drdy_fd == -1);
    assert(regs[ADS1256_REG_MUX] == 0x38);             /* AIN3 - AINCOM */
    assert((regs[ADS1256_REG_ADCON] & 0x07) == 3);     /* PGA 8 */
    assert(regs[ADS1256_REG_ADCON] == 0x03);           /* CLKOUT off (reset value 0x20), SDCS off */
    assert(regs[ADS1256_REG_STATUS] & ADS1256_STATUS_BUFFER_MASK);
    assert(regs[ADS1256_REG_DRATE] == 0xF0);
    assert(last_cmds[0] == ADS1256_CMD_SELFCAL && last_cmds[1] == ADS1256_CMD_RESET);

    /* Inputs: any pair, never pos == neg or out of range */
    assert(ads1256_set_input(&adc, ADS1256_AIN0, ADS1256_AIN1) == ADS1256_OK);
    assert(regs[ADS1256_REG_MUX] == 0x01);
    assert(ads1256_set_input(&adc, ADS1256_AIN2, ADS1256_AIN2) == ADS1256_ERROR_PARAMETER);
    assert(ads1256_set_input(&adc, 9, ADS1256_AIN0) == ADS1256_ERROR_PARAMETER);
    assert(regs[ADS1256_REG_MUX] == 0x01);

    /* Setters recalibrate */
    assert(ads1256_set_gain(&adc, ADS1256_GAIN_64) == ADS1256_OK);
    assert((regs[ADS1256_REG_ADCON] & 0x07) == 6 && last_cmds[0] == ADS1256_CMD_SELFCAL);
    assert(ads1256_set_gain(&adc, 3) == ADS1256_ERROR_PARAMETER);
    assert(ads1256_set_gain(&adc, ADS1256_GAIN_1) == ADS1256_OK);
    assert(ads1256_set_drate(&adc, ADS1256_DRATE_15000) == ADS1256_OK);
    assert(regs[ADS1256_REG_DRATE] == 0xE0 && last_cmds[0] == ADS1256_CMD_SELFCAL);
    assert(ads1256_set_buffer(&adc, false) == ADS1256_OK);
    assert(!(regs[ADS1256_REG_STATUS] & ADS1256_STATUS_BUFFER_MASK));

    /* Read restarts conversion, so the value always belongs to the current input */
    assert(ads1256_read(&adc, &raw) == ADS1256_OK);
    assert(last_cmds[0] == ADS1256_CMD_WAKEUP && last_cmds[1] == ADS1256_CMD_SYNC);

    /* Sample time without DRDY: WAKEUP + t18 / 2 (15000 SPS: t18 = 250 us) */
    uint64_t t_ns, t_start = mono_ns();
    assert(ads1256_read_ts(&adc, &raw, &t_ns) == ADS1256_OK);
    assert(t_ns >= t_start + 125000 && t_ns <= mono_ns() + 125000);
    assert(ads1256_read_ts(&adc, &raw, NULL) == ADS1256_OK);
    check_acquisition(&adc);
    uint8_t bad_inputs[2][2] = { { ADS1256_AIN0, ADS1256_AIN1 }, { ADS1256_AIN1, ADS1256_AIN1 } };
    int32_t values[2];
    assert(ads1256_scan(&adc, bad_inputs, 2, values) == ADS1256_ERROR_PARAMETER);

    /* Calibration: OFC signed, FSC unsigned, 3 bytes each, LSB in OFC0 / FSC0 */
    ads1256_calibration_t cal;
    regs[ADS1256_REG_OFC0] = 0xFE; regs[ADS1256_REG_OFC1] = 0xFF; regs[ADS1256_REG_OFC2] = 0xFF;  /* -2 */
    regs[ADS1256_REG_FSC0] = 0x08; regs[ADS1256_REG_FSC1] = 0xAC; regs[ADS1256_REG_FSC2] = 0x44;
    assert(ads1256_get_calibration(&adc, &cal) == ADS1256_OK);
    assert(cal.ofc == -2 && cal.fsc == 0x44AC08 && cal.gain == ADS1256_GAIN_1 &&
           cal.drate == ADS1256_DRATE_15000 && !cal.buffer && fabs(cal.full_scale - 5.0) < 1e-12);
    cal.ofc = -123456;                                 /* 0xFE1DC0 */
    cal.fsc = 0x400001;
    cal.full_scale = 4.0;                              /* V_cal after SYSGCAL */
    assert(ads1256_set_calibration(&adc, &cal) == ADS1256_OK);
    assert(regs[ADS1256_REG_OFC0] == 0xC0 && regs[ADS1256_REG_OFC1] == 0x1D && regs[ADS1256_REG_OFC2] == 0xFE);
    assert(regs[ADS1256_REG_FSC0] == 0x01 && regs[ADS1256_REG_FSC1] == 0x00 && regs[ADS1256_REG_FSC2] == 0x40);
    assert(adc.cfg.v_ref == 2.0 && fabs(ads1256_to_volts(&adc, 0x800000) - 4.0) < 1e-12);
    ads1256_calibration_t bad_cal = cal;
    bad_cal.drate = ADS1256_DRATE_2_5;                 /* Other settings: refused, nothing written */
    assert(ads1256_set_calibration(&adc, &bad_cal) == ADS1256_ERROR_PARAMETER);
    bad_cal = cal;
    bad_cal.ofc = 0x800000;
    assert(ads1256_set_calibration(&adc, &bad_cal) == ADS1256_ERROR_PARAMETER);
    bad_cal = cal;
    bad_cal.fsc = 0x1000000;
    assert(ads1256_set_calibration(&adc, &bad_cal) == ADS1256_ERROR_PARAMETER);
    bad_cal = cal;
    bad_cal.full_scale = NAN;
    assert(ads1256_set_calibration(&adc, &bad_cal) == ADS1256_ERROR_PARAMETER);
    assert(ads1256_set_calibration(&adc, NULL) == ADS1256_ERROR_PARAMETER);
    assert(regs[ADS1256_REG_OFC2] == 0xFE && adc.cfg.v_ref == 2.0);
    adc.cfg.v_ref = 2.5;

    /* Volts: full scale is +-2 * Vref / gain */
    assert(fabs(ads1256_to_volts(&adc, 0x400000) - 2.5) < 1e-9);
    assert(fabs(ads1256_to_volts(&adc, -0x400000) + 2.5) < 1e-9);
    assert(ads1256_sps(ADS1256_DRATE_2_5) == 2.5f && ads1256_sps(16) == 0.0f);
    assert(strcmp(ads1256_strerror(ADS1256_ERROR_TIMEOUT), "Timeout expired") == 0);
    assert(strcmp(ads1256_strerror(-9), "Unknown error") == 0);

    ads1256_close(&adc);
    assert(adc.spi_fd == -1 && adc.drdy_fd == -1);

    /* ===== DRDY on GPIO ===== */

    cfg.drdy_chip = "/dev/null";                       /* Line request goes to fake ioctl */
    cfg.drdy_line = 22;
    cfg.drate = ADS1256_DRATE_100;                     /* 10 ms period: real time can't make it late */
    cfg.timeout_ms = 1;                                /* Calibration must not use it */
    assert(ads1256_open(&adc, &cfg) == ADS1256_OK);
    assert(adc.drdy_fd == gpio_rd);
    check_acquisition(&adc);
    assert(last_cmds[0] == ADS1256_CMD_SDATAC && duplex_stops == 1);  /* Stopped in the last read */
    assert(last_cmds[1] != ADS1256_CMD_SDATAC);                       /* No separate SDATAC */

    /* One sample: plain RDATA, no RDATAC to leave */
    int32_t one;
    assert(ads1256_read_stream(&adc, &one, 1, NULL) == ADS1256_OK && one == code_for(0x45));
    assert(!rdatac && last_cmds[0] == ADS1256_CMD_WAKEUP && duplex_stops == 1);

    /* Sample time with DRDY: centre of the window, t18 / 2 = 5.09 ms before the edge (100 SPS) */
    assert(ads1256_read_ts(&adc, &raw, &t_ns) == ADS1256_OK && t_ns == last_edge_ns - 5090000);
    /* Read 2.5 periods late: the register holds the conversion two periods later
     * (10 SPS: 50 ms from both period boundaries, so a loaded machine can't change the result) */
    assert(ads1256_set_drate(&adc, ADS1256_DRATE_10) == ADS1256_OK);
    slow_level_us = 250000;
    assert(ads1256_read_ts(&adc, &raw, &t_ns) == ADS1256_OK && t_ns == last_edge_ns + 200000000 - 50090000);
    slow_level_us = 0;
    assert(ads1256_set_drate(&adc, ADS1256_DRATE_100) == ADS1256_OK);

    /* Signal while waiting for DRDY is not an error */
    int32_t stream[5];
    size_t count;
    poll_eintr = 1;
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_OK && count == 5);
    assert(!poll_eintr);

    /* Late-delivered events of old conversions are ignored */
    stale_edge = 1;
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_OK && count == 5);
    stale_edge = 0;

    /* Skipped conversion is reported (here before RDATAC was even sent) */
    edges_per_conversion = 2;
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_OVERRUN && count == 0);
    assert(!rdatac && last_cmds[0] == ADS1256_CMD_WAKEUP);
    edges_per_conversion = 1;

    /* Read finishing late in the conversion period is reported, RDATAC is left */
    slow_read_us = 10000;                              /* Period is 10 ms */
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_OVERRUN && count == 0);
    assert(!rdatac && last_cmds[0] == ADS1256_CMD_SDATAC);
    slow_read_us = 0;

    /* Late last read loses its SDATAC: the separate SDATAC still leaves RDATAC */
    int stops = duplex_stops;
    slow_last_read = 1;
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_OVERRUN && count == 4);
    assert(!rdatac && last_cmds[0] == ADS1256_CMD_SDATAC && duplex_stops == stops);
    slow_last_read = 0;

    /* DRDY stalls: timeout, and RDATAC is left even without a final DRDY */
    commits_left = 0;
    assert(ads1256_read(&adc, &raw) == ADS1256_ERROR_TIMEOUT);
    commits_left = 2;
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_TIMEOUT && count == 2);
    assert(!rdatac && last_cmds[0] == ADS1256_CMD_SDATAC);

    /* Noisy DRDY line: edges keep coming but DRDY stays high, the deadline still holds */
    noise = 1;
    commits_left = 0;
    assert(ads1256_read(&adc, &raw) == ADS1256_ERROR_TIMEOUT);
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_TIMEOUT && count == 0);
    noise = 0;

    /* GPIO line gone (POLLHUP): error instead of spinning */
    close(gpio_wr);
    assert(ads1256_read_stream(&adc, stream, 5, &count) == ADS1256_ERROR_COMMUNICATION && count == 0);
    commits_left = -1;

    ads1256_close(&adc);
    assert(adc.spi_fd == -1 && adc.drdy_fd == -1);
    ads1256_close(NULL);

    puts("All tests passed");
    return 0;
}
