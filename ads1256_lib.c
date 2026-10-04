/**
 * @file ads1256_lib.c
 * @brief Library for ADS1256 24-bit ADC on Linux spidev
 * @version 4.0
 * @date 2026-10-04
 *
 * Datasheet: TI SBAS288K. Timing constants assume fCLKIN = 7.68 MHz.
 * Uses only Linux spidev and GPIO character device (uAPI v2, kernel >= 5.10).
 */

#define _POSIX_C_SOURCE 200809L   /* clock_nanosleep, O_CLOEXEC also under strict -std=c11 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>

#include "ads1256_lib.h"

#define T6_DELAY_US     7         /* RDATA/RDATAC/RREG -> data: 50 / fCLKIN = 6.5 us */
#define T10_DELAY_US    2         /* Last SCLK -> CS high: 8 / fCLKIN = 1.04 us */
#define SYNC_DELAY_US   4         /* SYNC -> WAKEUP: 24 / fCLKIN = 3.1 us */
#define RESET_DELAY_US  10000
#define MAX_SPI_SPEED_HZ 1920000  /* fCLKIN / 4 */

/* Data rates [SPS] and DRATE register values */
static const float SPS[16] = {
    30000, 15000, 7500, 3750, 2000, 1000, 500, 100, 60, 50, 30, 25, 15, 10, 5, 2.5f
};
static const uint8_t DRATE_REG[16] = {
    0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA1, 0x92, 0x82,
    0x72, 0x63, 0x53, 0x43, 0x33, 0x23, 0x13, 0x03
};

/* Calibration times [us], datasheet tables 19 and 21 (worst PGA case);
 * gain calibrations (tables 20, 22) are shorter than SELFCAL */
static const uint32_t SELFCAL_US[16] = {
    892, 896, 1029, 1300, 2000, 3600, 6600, 31200,
    50900, 61800, 101300, 123200, 202100, 307200, 613800, 1227200
};
static const uint32_t OFFSETCAL_US[16] = {
    387, 453, 587, 853, 1300, 2300, 4300, 20300,
    33700, 40300, 67000, 80300, 133700, 200300, 400300, 800300
};


/* ===== Time ===== */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/** Sleep the whole time even when signals arrive (usleep may return early or reject >= 1 s) */
static void sleep_us(uint32_t us)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t ns = (uint64_t)t.tv_nsec + (uint64_t)us * 1000u;
    t.tv_sec += (time_t)(ns / 1000000000u);
    t.tv_nsec = (long)(ns % 1000000000u);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {
    }
}

static uint64_t period_ns(const ads1256_t *dev)
{
    return (uint64_t)(1e9 / SPS[dev->cfg.drate]);
}

/** Timeout for one conversion: timeout_ms on top of 2 periods (settling after SYNC) */
static uint32_t data_timeout_ms(const ads1256_t *dev)
{
    return dev->cfg.timeout_ms + (uint32_t)(2 * period_ns(dev) / 1000000u) + 1;
}


/* ===== SPI helpers ===== */

/** Open spidev in SPI mode 1 with given clock, returns fd or -1 */
static int open_spi(const char *device, uint32_t speed_hz)
{
    uint8_t mode = SPI_MODE_1;
    int fd = open(device, O_RDWR | O_CLOEXEC);

    if (fd >= 0 && (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
                    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * One SPI message with CS low throughout: tx_len bytes from tx, then rx_len bytes
 * into rx (zeros are sent). Between them waits t6 (RDATA, RDATAC, RREG), at the
 * end t10 before CS goes high. Either part may be empty.
 */
static int transfer(ads1256_t *dev, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    struct spi_ioc_transfer xfer[2] = { 0 };
    unsigned int n = 0;

    if (tx_len) {
        xfer[n++] = (struct spi_ioc_transfer){ .tx_buf = (unsigned long)tx, .len = tx_len,
                                               .delay_usecs = T6_DELAY_US };
    }
    if (rx_len) {
        xfer[n++] = (struct spi_ioc_transfer){ .rx_buf = (unsigned long)rx, .len = rx_len };
    }
    xfer[n - 1].delay_usecs = T10_DELAY_US;

    return ioctl(dev->spi_fd, SPI_IOC_MESSAGE(n), xfer) < 0 ? ADS1256_ERROR_COMMUNICATION : ADS1256_OK;
}

static int send_command(ads1256_t *dev, uint8_t cmd)
{
    return transfer(dev, &cmd, 1, NULL, 0);
}

/** Signed 24-bit value from three bytes, MSB first */
static int32_t get_24bit_value(const uint8_t *d)
{
    int32_t v = (int32_t)d[0] << 16 | (int32_t)d[1] << 8 | d[2];
    return (v ^ 0x800000) - 0x800000;
}

static int read_data(ads1256_t *dev, int32_t *raw)
{
    const uint8_t cmd = ADS1256_CMD_RDATA;
    uint8_t d[3];
    int result = transfer(dev, &cmd, 1, d, 3);
    if (result == ADS1256_OK) {
        *raw = get_24bit_value(d);
    }
    return result;
}

static int update_register_bits(ads1256_t *dev, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old;
    int result = ads1256_read_register(dev, reg, &old);
    if (result != ADS1256_OK) {
        return result;
    }
    return ads1256_write_register(dev, reg, (uint8_t)((old & ~mask) | (value & mask)));
}

/** Restart conversion, next DRDY then delivers settled data with current settings */
static int start_conversion(ads1256_t *dev)
{
    int result = send_command(dev, ADS1256_CMD_SYNC);
    if (result != ADS1256_OK) {
        return result;
    }
    sleep_us(SYNC_DELAY_US);
    return send_command(dev, ADS1256_CMD_WAKEUP);
}


/* ===== Waiting for DRDY ===== */

/** Open DRDY GPIO line as input with falling edge events, returns line fd or -1 */
static int open_drdy(const char *chip, unsigned int line)
{
    int chip_fd = open(chip, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        return -1;
    }

    struct gpio_v2_line_request req = {
        .offsets = { line },
        .consumer = "ads1256-drdy",
        .config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_FALLING,
        .num_lines = 1,
    };
    int result = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip_fd);  /* Line fd stays valid */
    if (result < 0) {
        return -1;
    }

    if (fcntl(req.fd, F_SETFL, O_NONBLOCK) < 0) {  /* For draining queued events */
        close(req.fd);
        return -1;
    }
    return req.fd;
}

/**
 * Read all queued edge events without blocking. Counts only events not older than
 * `after` (older ones were delivered late for conversions already handled; equal
 * timestamps can occur within the clock resolution) and
 * stores the newest kernel timestamp (CLOCK_MONOTONIC) in *newest.
 * Returns the count, or -1 on read error.
 */
static int drain_edges(int fd, uint64_t after, uint64_t *newest)
{
    struct gpio_v2_line_event events[16];
    int count = 0;
    ssize_t len;

    while ((len = read(fd, events, sizeof(events))) > 0) {
        for (size_t i = 0; i < (size_t)len / sizeof(events[0]); i++) {
            if (events[i].timestamp_ns >= after) {
                count++;
                if (newest) {
                    *newest = events[i].timestamp_ns;
                }
            }
        }
    }
    return len < 0 && errno != EAGAIN ? -1 : count;
}

static int drdy_level(ads1256_t *dev, int *level)
{
    struct gpio_v2_line_values values = { .mask = 1 };
    if (ioctl(dev->drdy_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    *level = (int)(values.bits & 1);
    return ADS1256_OK;
}

/** Wait until an edge event is queued or the deadline (now_ns time) passes, retried after signals */
static int poll_edge(ads1256_t *dev, uint64_t deadline)
{
    struct pollfd pfd = { .fd = dev->drdy_fd, .events = POLLIN };
    int result;

    do {
        uint64_t now = now_ns();
        int ms = now >= deadline ? 0 : (int)((deadline - now + 999999) / 1000000);
        result = poll(&pfd, 1, ms);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        return ADS1256_ERROR_TIMEOUT;
    }
    if (result < 0 || !(pfd.revents & POLLIN)) {
        return ADS1256_ERROR_COMMUNICATION;  /* POLLERR/POLLHUP: line is gone */
    }
    return ADS1256_OK;
}

/**
 * Wait for DRDY low on the GPIO pin; the level is checked, so stale edges don't matter.
 * The deadline is fixed, so a noisy line (edges, but no low level) still times out.
 */
static int wait_drdy_pin(ads1256_t *dev, uint32_t timeout_ms)
{
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000u;

    for (;;) {
        int level, result;
        if (drain_edges(dev->drdy_fd, 0, NULL) < 0) {
            return ADS1256_ERROR_COMMUNICATION;
        }
        if ((result = drdy_level(dev, &level)) != ADS1256_OK || !level) {
            return result;  /* DRDY is active low */
        }
        if (now_ns() >= deadline) {
            return ADS1256_ERROR_TIMEOUT;  /* Edges keep coming, DRDY doesn't stay low */
        }
        if ((result = poll_edge(dev, deadline)) != ADS1256_OK) {
            return result;
        }
    }
}

/** Wait for falling edges at or after `after` (streaming); edges gets their count, *newest the last time */
static int wait_edges(ads1256_t *dev, uint64_t after, int *edges, uint64_t *newest)
{
    uint64_t deadline = now_ns() + (uint64_t)data_timeout_ms(dev) * 1000000u;

    do {
        int result = poll_edge(dev, deadline);
        if (result != ADS1256_OK) {
            return result;
        }
        if ((*edges = drain_edges(dev->drdy_fd, after, newest)) < 0) {
            return ADS1256_ERROR_COMMUNICATION;
        }
        if (*edges == 0 && now_ns() >= deadline) {
            return ADS1256_ERROR_TIMEOUT;  /* Only old or noise edges came */
        }
    } while (*edges == 0);
    return ADS1256_OK;
}

/**
 * Wait for DRDY bit in STATUS register.
 * Sleeps 70 % of the conversion period first, then polls at an interval based on data rate.
 */
static int wait_drdy_poll(ads1256_t *dev)
{
    uint64_t deadline = now_ns() + (uint64_t)data_timeout_ms(dev) * 1000000u;

    uint32_t sleep = (uint32_t)(period_ns(dev) * 7 / 10 / 1000);
    if (sleep > 100) {
        sleep_us(sleep);
    }

    uint32_t poll_us = dev->cfg.drate <= ADS1256_DRATE_1000 ? 20 :
                       dev->cfg.drate <= ADS1256_DRATE_100 ? 100 : 500;

    for (;;) {
        uint8_t status;
        int result = ads1256_read_register(dev, ADS1256_REG_STATUS, &status);
        if (result != ADS1256_OK) {
            return result;
        }
        if (!(status & ADS1256_STATUS_DRDY_MASK)) {
            return ADS1256_OK;
        }

        sleep_us(poll_us);
        if (now_ns() > deadline) {
            return ADS1256_ERROR_TIMEOUT;
        }
    }
}

static int wait_drdy(ads1256_t *dev)
{
    return dev->drdy_fd >= 0 ? wait_drdy_pin(dev, data_timeout_ms(dev)) : wait_drdy_poll(dev);
}

/**
 * Wait for the end of calibration taking up to `us` per datasheet.
 * ponytail: until DRDY goes low no command may be sent, not even RREG of STATUS,
 * so without DRDY pin sleep the datasheet time + 10 % margin (the tables
 * don't say typical or maximum). Upgrade: wire DRDY.
 */
static int wait_ready(ads1256_t *dev, uint32_t us)
{
    us += us / 10;
    if (dev->drdy_fd >= 0) {
        return wait_drdy_pin(dev, us / 1000 + data_timeout_ms(dev));  /* Up to 1.2 s at 2.5 SPS */
    }
    sleep_us(us);
    return ADS1256_OK;
}


/* ===== Configuration ===== */

static bool valid_input(uint8_t pos, uint8_t neg)
{
    return pos <= ADS1256_AINCOM && neg <= ADS1256_AINCOM && pos != neg;
}

/** PGA register bits = log2(gain), -1 for invalid gain */
static int pga_bits(int gain)
{
    for (int bits = 0; bits <= 6; bits++) {
        if (gain == 1 << bits) {
            return bits;
        }
    }
    return -1;
}

int ads1256_open(ads1256_t *dev, const ads1256_config_t *cfg)
{
    if (!dev) {
        return ADS1256_ERROR_PARAMETER;
    }
    dev->spi_fd = dev->drdy_fd = -1;  /* ads1256_close() is safe after any failure */

    if (!cfg || !cfg->spi_device ||
        cfg->spi_speed_hz == 0 || cfg->spi_speed_hz > MAX_SPI_SPEED_HZ ||
        !(cfg->v_ref >= 0.5 && cfg->v_ref <= 2.6) ||  /* Also rejects NaN */
        (unsigned)cfg->drate > ADS1256_DRATE_2_5 || pga_bits(cfg->gain) < 0 ||
        !valid_input(cfg->pos, cfg->neg) ||
        cfg->timeout_ms == 0 || cfg->timeout_ms > 3600000) {
        return ADS1256_ERROR_PARAMETER;
    }

    dev->cfg = *cfg;
    dev->spi_fd = open_spi(cfg->spi_device, cfg->spi_speed_hz);
    if (dev->spi_fd < 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    if (cfg->drdy_chip && (dev->drdy_fd = open_drdy(cfg->drdy_chip, cfg->drdy_line)) < 0) {
        ads1256_close(dev);
        return ADS1256_ERROR_COMMUNICATION;
    }

    /* Fixed delay after RESET: with the pin, DRDY may still be low from old data */
    int result = send_command(dev, ADS1256_CMD_RESET);
    sleep_us(RESET_DELAY_US);

    /* Write registers directly and calibrate once (setters would calibrate each time) */
    if (result != ADS1256_OK ||
        (result = ads1256_write_register(dev, ADS1256_REG_STATUS,
                                         cfg->buffer ? ADS1256_STATUS_BUFFER_MASK : 0)) != ADS1256_OK ||
        (result = ads1256_write_register(dev, ADS1256_REG_MUX,
                                         (uint8_t)(cfg->pos << 4 | cfg->neg))) != ADS1256_OK ||
        (result = update_register_bits(dev, ADS1256_REG_ADCON, ADS1256_ADCON_GAIN_MASK,
                                       (uint8_t)pga_bits(cfg->gain))) != ADS1256_OK ||
        (result = ads1256_write_register(dev, ADS1256_REG_DRATE, DRATE_REG[cfg->drate])) != ADS1256_OK ||
        (result = ads1256_calibrate(dev, ADS1256_CMD_SELFCAL)) != ADS1256_OK) {
        ads1256_close(dev);
        return result;
    }

    return ADS1256_OK;
}

void ads1256_close(ads1256_t *dev)
{
    if (!dev) {
        return;
    }
    if (dev->drdy_fd >= 0) {
        close(dev->drdy_fd);
    }
    if (dev->spi_fd >= 0) {
        close(dev->spi_fd);
    }
    dev->spi_fd = dev->drdy_fd = -1;
}

int ads1256_set_input(ads1256_t *dev, uint8_t pos, uint8_t neg)
{
    if (!valid_input(pos, neg)) {
        return ADS1256_ERROR_PARAMETER;
    }

    /* MUX = PSEL (bits 7-4) | NSEL (bits 3-0) */
    int result = ads1256_write_register(dev, ADS1256_REG_MUX, (uint8_t)(pos << 4 | neg));
    if (result == ADS1256_OK) {
        dev->cfg.pos = pos;
        dev->cfg.neg = neg;
    }
    return result;
}

int ads1256_set_gain(ads1256_t *dev, ads1256_gain_t gain)
{
    int pga = pga_bits(gain);
    if (pga < 0) {
        return ADS1256_ERROR_PARAMETER;
    }

    int result = update_register_bits(dev, ADS1256_REG_ADCON, ADS1256_ADCON_GAIN_MASK, (uint8_t)pga);
    if (result != ADS1256_OK) {
        return result;
    }
    dev->cfg.gain = gain;
    return ads1256_calibrate(dev, ADS1256_CMD_SELFCAL);
}

int ads1256_set_drate(ads1256_t *dev, ads1256_drate_t drate)
{
    if ((unsigned)drate > ADS1256_DRATE_2_5) {
        return ADS1256_ERROR_PARAMETER;
    }

    int result = ads1256_write_register(dev, ADS1256_REG_DRATE, DRATE_REG[drate]);
    if (result != ADS1256_OK) {
        return result;
    }
    dev->cfg.drate = drate;  /* Calibration time depends on the new rate */
    return ads1256_calibrate(dev, ADS1256_CMD_SELFCAL);
}

int ads1256_set_buffer(ads1256_t *dev, bool on)
{
    int result = update_register_bits(dev, ADS1256_REG_STATUS, ADS1256_STATUS_BUFFER_MASK,
                                      on ? ADS1256_STATUS_BUFFER_MASK : 0);
    if (result != ADS1256_OK) {
        return result;
    }
    dev->cfg.buffer = on;
    return ads1256_calibrate(dev, ADS1256_CMD_SELFCAL);
}

int ads1256_calibrate(ads1256_t *dev, uint8_t cmd)
{
    if (cmd < ADS1256_CMD_SELFCAL || cmd > ADS1256_CMD_SYSGCAL) {
        return ADS1256_ERROR_PARAMETER;
    }

    int result = send_command(dev, cmd);
    if (result != ADS1256_OK) {
        return result;
    }

    bool offset_only = cmd == ADS1256_CMD_SELFOCAL || cmd == ADS1256_CMD_SYSOCAL;
    return wait_ready(dev, (offset_only ? OFFSETCAL_US : SELFCAL_US)[dev->cfg.drate]);
}


/* ===== Data acquisition ===== */

int ads1256_read(ads1256_t *dev, int32_t *raw)
{
    if (!raw) {
        return ADS1256_ERROR_PARAMETER;
    }

    int result;
    if ((result = start_conversion(dev)) != ADS1256_OK ||
        (result = wait_drdy(dev)) != ADS1256_OK) {
        return result;
    }
    return read_data(dev, raw);
}

/**
 * Stream with RDATAC, DRDY on GPIO, started at time `edge` (after SYNC+WAKEUP).
 * Each sample needs exactly one new falling edge, judged by kernel timestamps,
 * so late-delivered events of earlier conversions don't count. More edges, or a
 * read finishing late in the conversion period, mean a conversion was lost or
 * the data could be overwritten while read: ADS1256_ERROR_OVERRUN.
 * ponytail: the 90 % limit assumes the nominal data rate (fCLKIN = 7.68 MHz).
 */
static int read_stream_pin(ads1256_t *dev, int32_t *raw, size_t n, size_t *done, uint64_t edge)
{
    const uint8_t cmd = ADS1256_CMD_RDATAC;
    const uint64_t limit = period_ns(dev) * 9 / 10;
    bool rdatac = false;
    int edges, result;
    uint8_t d[3];

    do {
        if ((result = wait_edges(dev, edge, &edges, &edge)) != ADS1256_OK) {
            break;
        }
        if (edges > 1) {
            result = ADS1256_ERROR_OVERRUN;  /* A conversion finished unread */
            break;
        }
        if (!rdatac) {
            rdatac = true;
            result = transfer(dev, &cmd, 1, d, 3);  /* First result comes with RDATAC */
        } else {
            result = transfer(dev, NULL, 0, d, 3);  /* Then 24 bits are clocked out after each DRDY */
        }
        if (result == ADS1256_OK && now_ns() - edge > limit) {
            result = ADS1256_ERROR_OVERRUN;  /* Read may overlap the next update */
        }
        if (result == ADS1256_OK) {
            raw[(*done)++] = get_24bit_value(d);
        }
    } while (result == ADS1256_OK && *done < n);

    if (!rdatac) {
        return result;
    }

    /* Leave RDATAC: SDATAC must follow a DRDY falling edge before the next update, so wait
     * for a fresh edge (after an overrun DRDY may have been low for most of the period).
     * After a timeout there is no edge to wait for, send it anyway. */
    int stop = ADS1256_OK;
    if (result != ADS1256_ERROR_TIMEOUT) {
        stop = wait_edges(dev, now_ns(), &edges, &edge);
    }
    int sdatac = send_command(dev, ADS1256_CMD_SDATAC);
    if (stop == ADS1256_OK) {
        stop = sdatac;
    }
    return result != ADS1256_OK ? result : stop;
}

int ads1256_read_stream(ads1256_t *dev, int32_t *raw, size_t n, size_t *count)
{
    size_t done = 0;

    if (count) {
        *count = 0;
    }
    if (!raw) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (n == 0) {
        return ADS1256_OK;
    }

    int result = start_conversion(dev);
    uint64_t started = now_ns();  /* Edges before this belong to old conversions */

    if (result == ADS1256_OK && dev->drdy_fd >= 0) {
        result = read_stream_pin(dev, raw, n, &done, started);
    } else {
        /* ponytail: RDATAC needs the DRDY pin (STATUS can't be read in RDATAC mode),
         * so each sample is a STATUS poll + RDATA. Throughput ~1-2 kSPS; upgrade: wire DRDY. */
        while (result == ADS1256_OK && done < n &&
               (result = wait_drdy(dev)) == ADS1256_OK &&
               (result = read_data(dev, &raw[done])) == ADS1256_OK) {
            done++;
        }
    }

    if (count) {
        *count = done;
    }
    return result;
}

int ads1256_scan(ads1256_t *dev, uint8_t inputs[][2], size_t n, int32_t *raw)
{
    if (!inputs || !raw) {
        return ADS1256_ERROR_PARAMETER;
    }
    for (size_t i = 0; i < n; i++) {
        if (!valid_input(inputs[i][0], inputs[i][1])) {
            return ADS1256_ERROR_PARAMETER;
        }
    }
    if (n == 0) {
        return ADS1256_OK;
    }

    int result;
    if ((result = ads1256_set_input(dev, inputs[0][0], inputs[0][1])) != ADS1256_OK ||
        (result = start_conversion(dev)) != ADS1256_OK) {
        return result;
    }

    /* Datasheet "cycling": after DRDY switch MUX and restart, then read the previous input */
    for (size_t i = 0; i < n; i++) {
        if ((result = wait_drdy(dev)) != ADS1256_OK) {
            return result;
        }
        if (i + 1 < n &&
            ((result = ads1256_set_input(dev, inputs[i + 1][0], inputs[i + 1][1])) != ADS1256_OK ||
             (result = start_conversion(dev)) != ADS1256_OK)) {
            return result;
        }
        if ((result = read_data(dev, &raw[i])) != ADS1256_OK) {
            return result;
        }
    }
    return ADS1256_OK;
}

double ads1256_to_volts(const ads1256_t *dev, int32_t raw)
{
    /* Full scale +-2 * Vref / gain at code +-2^23 */
    return 2.0 * dev->cfg.v_ref / dev->cfg.gain * raw / 0x800000;
}

float ads1256_sps(ads1256_drate_t drate)
{
    return (unsigned)drate <= ADS1256_DRATE_2_5 ? SPS[drate] : 0.0f;
}


/* ===== Registers ===== */

int ads1256_read_register(ads1256_t *dev, uint8_t reg, uint8_t *value)
{
    if (reg > ADS1256_REG_FSC2 || !value) {
        return ADS1256_ERROR_PARAMETER;
    }

    const uint8_t cmd[2] = { ADS1256_CMD_RREG | reg, 0x00 };  /* Read one register */
    return transfer(dev, cmd, 2, value, 1);
}

int ads1256_write_register(ads1256_t *dev, uint8_t reg, uint8_t value)
{
    if (reg > ADS1256_REG_FSC2) {
        return ADS1256_ERROR_PARAMETER;
    }

    const uint8_t cmd[3] = { ADS1256_CMD_WREG | reg, 0x00, value };  /* Write one register */
    return transfer(dev, cmd, 3, NULL, 0);
}

const char *ads1256_strerror(int error_code)
{
    static const char *messages[] = {
        "Success", "Invalid parameter", "Communication error", "Timeout expired",
        "Conversions skipped (reading too slow)"
    };

    if (error_code > 0 || error_code < ADS1256_ERROR_OVERRUN) {
        return "Unknown error";
    }
    return messages[-error_code];
}
