/**
 * @file ads1256_lib.h
 * @brief Library for ADS1256 24-bit ADC on Linux spidev (Orange Pi, Raspberry Pi)
 * @version 4.6
 * @date 2026-10-07
 * Changes: see Version History in README.md
 *
 * Wiring (Orange Pi 5 / Raspberry Pi header):
 *  ADS1256   Header
 *  ---------------------
 *  CS        CE0   (24)
 *  DOUT      MISO  (21)
 *  DIN       MOSI  (19)
 *  SCLK      SCLK  (23)
 *  GND       GND   (6,9,14,20,25,30,34,39)
 *  5V        5V    (2)
 *  DRDY      any GPIO, optional (see ads1256_config_t.drdy_chip)
 */

#ifndef ADS1256_LIB_H
#define ADS1256_LIB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Return codes */
#define ADS1256_OK                   0  /**< Operation successful */
#define ADS1256_ERROR_PARAMETER     -1  /**< Invalid parameter */
#define ADS1256_ERROR_COMMUNICATION -2  /**< SPI or GPIO error */
#define ADS1256_ERROR_TIMEOUT       -3  /**< DRDY did not go low in time */
#define ADS1256_ERROR_OVERRUN       -4  /**< read_stream() couldn't keep up, conversions skipped */

/* Registers */
#define ADS1256_REG_STATUS       0x00   /**< Status register */
#define ADS1256_REG_MUX          0x01   /**< Input multiplexer register */
#define ADS1256_REG_ADCON        0x02   /**< A/D Control register */
#define ADS1256_REG_DRATE        0x03   /**< A/D Data rate register */
#define ADS1256_REG_IO           0x04   /**< GPIO Control register */
#define ADS1256_REG_OFC0         0x05   /**< Offset Calibration Coefficient 0 */
#define ADS1256_REG_OFC1         0x06   /**< Offset Calibration Coefficient 1 */
#define ADS1256_REG_OFC2         0x07   /**< Offset Calibration Coefficient 2 */
#define ADS1256_REG_FSC0         0x08   /**< Full-Scale Calibration Coefficient 0 */
#define ADS1256_REG_FSC1         0x09   /**< Full-Scale Calibration Coefficient 1 */
#define ADS1256_REG_FSC2         0x0A   /**< Full-Scale Calibration Coefficient 2 */

/* Register bit masks */
#define ADS1256_STATUS_DRDY_MASK     0x01   /**< DRDY bit in STATUS register */
#define ADS1256_STATUS_BUFFER_MASK   0x02   /**< Buffer enable bit in STATUS */
#define ADS1256_ADCON_GAIN_MASK      0x07   /**< Gain bits in ADCON (bits 0-2) */

/* Commands */
#define ADS1256_CMD_WAKEUP       0x00   /**< Wake up / complete SYNC */
#define ADS1256_CMD_RDATA        0x01   /**< Read data */
#define ADS1256_CMD_RDATAC       0x03   /**< Read data continuously */
#define ADS1256_CMD_SDATAC       0x0F   /**< Stop continuous data reading */
#define ADS1256_CMD_RREG         0x10   /**< Read register */
#define ADS1256_CMD_WREG         0x50   /**< Write to register */
#define ADS1256_CMD_SELFCAL      0xF0   /**< Self offset and gain calibration */
#define ADS1256_CMD_SELFOCAL     0xF1   /**< Self offset calibration */
#define ADS1256_CMD_SELFGCAL     0xF2   /**< Self gain calibration */
#define ADS1256_CMD_SYSOCAL      0xF3   /**< System offset calibration */
#define ADS1256_CMD_SYSGCAL      0xF4   /**< System gain calibration */
#define ADS1256_CMD_SYNC         0xFC   /**< Synchronization */
#define ADS1256_CMD_STANDBY      0xFD   /**< Enter standby mode */
#define ADS1256_CMD_RESET        0xFE   /**< Reset */

/* Analog inputs for ads1256_set_input() */
enum {
    ADS1256_AIN0 = 0, ADS1256_AIN1, ADS1256_AIN2, ADS1256_AIN3,
    ADS1256_AIN4, ADS1256_AIN5, ADS1256_AIN6, ADS1256_AIN7,
    ADS1256_AINCOM            /**< Common input for single-ended measurement */
};

/* Data rates */
typedef enum {
    ADS1256_DRATE_30000 = 0, ADS1256_DRATE_15000, ADS1256_DRATE_7500, ADS1256_DRATE_3750,
    ADS1256_DRATE_2000, ADS1256_DRATE_1000, ADS1256_DRATE_500, ADS1256_DRATE_100,
    ADS1256_DRATE_60, ADS1256_DRATE_50, ADS1256_DRATE_30, ADS1256_DRATE_25,
    ADS1256_DRATE_15, ADS1256_DRATE_10, ADS1256_DRATE_5, ADS1256_DRATE_2_5
} ads1256_drate_t;

/* Gain */
typedef enum {
    ADS1256_GAIN_1 = 1, ADS1256_GAIN_2 = 2, ADS1256_GAIN_4 = 4, ADS1256_GAIN_8 = 8,
    ADS1256_GAIN_16 = 16, ADS1256_GAIN_32 = 32, ADS1256_GAIN_64 = 64
} ads1256_gain_t;

/* Configuration for ads1256_open() */
typedef struct {
    const char *spi_device;    /**< SPI device, e.g. "/dev/spidev0.0" */
    uint32_t spi_speed_hz;     /**< SCLK, max 1920000 (fCLKIN/4) */
    const char *drdy_chip;     /**< GPIO chip with DRDY, e.g. "/dev/gpiochip1"; NULL = poll STATUS */
    unsigned int drdy_line;    /**< GPIO line offset of DRDY on drdy_chip */
    double v_ref;              /**< Reference voltage [V], 0.5-2.6 */
    ads1256_drate_t drate;     /**< Data rate */
    ads1256_gain_t gain;       /**< PGA gain */
    uint8_t pos, neg;          /**< Inputs: ADS1256_AIN0..ADS1256_AIN7 or ADS1256_AINCOM */
    bool buffer;               /**< Input buffer */
    uint32_t timeout_ms;       /**< DRDY timeout on top of 2 conversion periods, 1..3600000 */
} ads1256_config_t;

/* Device handle, owned by the caller; cfg always holds the current settings */
typedef struct {
    int spi_fd;
    int drdy_fd;               /**< -1 when DRDY pin is not used */
    ads1256_config_t cfg;
} ads1256_t;

/* Calibration coefficients for ads1256_get_calibration() / ads1256_set_calibration() */
typedef struct {
    int32_t ofc;               /**< OFC register, -2^23 .. 2^23-1 */
    uint32_t fsc;              /**< FSC register, 0 .. 2^24-1 */
    double full_scale;         /**< Input [V] that reads as code 2^23: 2 * v_ref / gain, or V_cal after SYSGCAL */
    ads1256_gain_t gain;       /**< Settings the coefficients belong to */
    ads1256_drate_t drate;
    bool buffer;
} ads1256_calibration_t;

/**
 * Open SPI (and DRDY GPIO), reset the ADC, apply configuration, self-calibrate.
 * Turns D0/CLKOUT off (datasheet: recommended when unused); to clock another chip from it,
 * write ADCON bits 6-5 afterwards with ads1256_write_register(), the setters keep them.
 * Call ads1256_close() before opening the same handle again, otherwise its file
 * descriptors leak (and the GPIO line stays busy).
 * @return ADS1256_OK or negative error; on error nothing stays open
 */
int ads1256_open(ads1256_t *dev, const ads1256_config_t *cfg);

/** Close SPI and GPIO file descriptors */
void ads1256_close(ads1256_t *dev);

/** Select inputs (pos != neg), e.g. AIN0/AIN1 differential or AIN3/AINCOM single-ended */
int ads1256_set_input(ads1256_t *dev, uint8_t pos, uint8_t neg);

/** Set PGA gain and self-calibrate */
int ads1256_set_gain(ads1256_t *dev, ads1256_gain_t gain);

/** Set data rate and self-calibrate */
int ads1256_set_drate(ads1256_t *dev, ads1256_drate_t drate);

/** Enable/disable input buffer and self-calibrate */
int ads1256_set_buffer(ads1256_t *dev, bool on);

/** Run calibration command (ADS1256_CMD_SELFCAL .. ADS1256_CMD_SYSGCAL) and wait for it */
int ads1256_calibrate(ads1256_t *dev, uint8_t cmd);

/** One fresh conversion with current settings (SYNC+WAKEUP, wait, RDATA), raw signed 24-bit code */
int ads1256_read(ads1256_t *dev, int32_t *raw);

/**
 * Like ads1256_read(), plus the sample time t_ns (optional): centre of the conversion window
 * (digital filter over t18 before DRDY, datasheet table 13) in CLOCK_MONOTONIC ns.
 * With DRDY pin from the kernel timestamp of the DRDY edge, without it from the time WAKEUP
 * was sent (tens of us). Above ~2000 SPS it may be one conversion period off (README).
 * Convert to wall clock in the application (offset to CLOCK_REALTIME).
 */
int ads1256_read_ts(ads1256_t *dev, int32_t *raw, uint64_t *t_ns);

/**
 * n consecutive conversions of the current input.
 * With DRDY pin uses RDATAC and returns ADS1256_ERROR_OVERRUN when a conversion
 * was skipped or read too late in its period (30 kSPS needs SCLK near 1.92 MHz
 * and may still not keep up). Without the pin each sample is a STATUS poll + RDATA and
 * skipped conversions are not detected; on the Orange Pi 5 that happens above 30 SPS (README).
 * count (optional) gets the number of valid samples in raw, also on error.
 */
int ads1256_read_stream(ads1256_t *dev, int32_t *raw, size_t n, size_t *count);

/**
 * One conversion of each input pair inputs[i] = { pos, neg } into raw[i].
 * Each input is restarted with SYNC, so its first conversion is settled; the result is
 * read before the next input is selected, so delays can't mix up inputs.
 * The last pair stays selected as the current input.
 */
int ads1256_scan(ads1256_t *dev, uint8_t inputs[][2], size_t n, int32_t *raw);  /* Not const: C < C23 */

/**
 * Read the current calibration (OFC, FSC) with the settings it belongs to; full_scale is
 * 2 * v_ref / gain. After ADS1256_CMD_SYSGCAL set full_scale to the applied voltage before saving.
 */
int ads1256_get_calibration(ads1256_t *dev, ads1256_calibration_t *cal);

/**
 * Write a saved calibration and set v_ref = full_scale * gain / 2, so ads1256_to_volts() stays
 * right after a system gain calibration. Refused (ADS1256_ERROR_PARAMETER) for other gain,
 * data rate or buffer than dev->cfg. Call after the setters: they self-calibrate over it.
 */
int ads1256_set_calibration(ads1256_t *dev, const ads1256_calibration_t *cal);

/**
 * Read a calibration file written by ads1256_cal (key=value lines, # comments).
 * path NULL = $XDG_CONFIG_HOME/ads1256/calibration.conf (~/.config/ads1256/calibration.conf).
 * Strict: a missing or repeated key, a value with trailing text or out of range, or a last line
 * without newline (file cut off) is an error. Unknown keys are ignored.
 * Apply it with ads1256_set_calibration(), which checks it belongs to the current settings.
 * @return ADS1256_OK, or ADS1256_ERROR_PARAMETER with errno ENOENT (no file), EINVAL (damaged
 *         or incomplete), another errno from fopen() or from ads1256_calibration_path()
 */
int ads1256_load_calibration(const char *path, ads1256_calibration_t *cal);

/**
 * Default calibration file into path: $XDG_CONFIG_HOME/ads1256/calibration.conf, without
 * XDG_CONFIG_HOME (or with a relative one) ~/.config/ads1256/calibration.conf.
 * Doesn't create the directories.
 * @return ADS1256_OK, or ADS1256_ERROR_PARAMETER with errno ENOENT (no HOME) or
 *         ENAMETOOLONG (path is then "")
 */
int ads1256_calibration_path(char *path, size_t size);

/** Convert raw code to volts using current v_ref and gain */
double ads1256_to_volts(const ads1256_t *dev, int32_t raw);

/** Data rate in samples per second (0 for invalid drate) */
float ads1256_sps(ads1256_drate_t drate);

/** Low-level register access; bypasses dev->cfg, use the setters for inputs, gain and data rate */
int ads1256_read_register(ads1256_t *dev, uint8_t reg, uint8_t *value);
int ads1256_write_register(ads1256_t *dev, uint8_t reg, uint8_t value);

/** Text description of a return code */
const char *ads1256_strerror(int error_code);

#endif /* ADS1256_LIB_H */
