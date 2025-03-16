/**
 * @file ads1256_lib.h
 * @brief Optimized library for ADS1256 24-bit ADC communication.
 * @version 3.1
 * @date 2025-03-16
 *
 * This library provides an interface for communicating with the ADS1256
 * 24-bit ADC converter via SPI on the Raspberry Pi platform.
 *
 * Wiring:
 *  ADS1256   RPi
 *  ---------------------
 *  CS        CE0   (24)
 *  DOUT      MISO  (21)
 *  DIN       MOSI  (19)
 *  SCLK      SCLK  (23)
 *  GND       GND   (6,9,14,20,25,30,34,39)
 *  5V        5V    (2) 
 *  DRDY      GPIO  (7)
 */

#ifndef ADS1256_LIB_H
#define ADS1256_LIB_H

#include <stdint.h>
#include <stdlib.h>

/* Helper macros for parameter validation */
#define CHECK_NULL_PARAM(param) if ((param) == NULL) return ADS1256_ERROR_PARAMETER
#define CHECK_RANGE_PARAM(param, min, max) if ((param) < (min) || (param) > (max)) return ADS1256_ERROR_PARAMETER

/* Return codes */
#define ADS1256_OK                 0    /**< Operation successful */
#define ADS1256_ERROR_PARAMETER   -1    /**< Invalid parameter */
#define ADS1256_ERROR_COMMUNICATION -2  /**< Communication error */
#define ADS1256_ERROR_MEMORY      -3    /**< Memory allocation error */
#define ADS1256_ERROR_TIMEOUT     -4    /**< Timeout expired */

/* ADS1256 Register definitions */
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

/* ADS1256 Commands */
#define ADS1256_CMD_WAKEUP       0x00   /**< Wake up from low power mode */
#define ADS1256_CMD_RDATA        0x01   /**< Read data */
#define ADS1256_CMD_RDATAC       0x03   /**< Read data continuously */
#define ADS1256_CMD_SDATAC       0x0F   /**< Stop continuous data reading */
#define ADS1256_CMD_RREG         0x10   /**< Read register */
#define ADS1256_CMD_WREG         0x50   /**< Write to register */
#define ADS1256_CMD_SELFCAL      0xF0   /**< Self calibration */
#define ADS1256_CMD_SELFOCAL     0xF1   /**< Self offset calibration */
#define ADS1256_CMD_SELFGCAL     0xF2   /**< Self gain calibration */
#define ADS1256_CMD_SYSOCAL      0xF3   /**< System offset calibration */
#define ADS1256_CMD_SYSGCAL      0xF4   /**< System gain calibration */
#define ADS1256_CMD_SYNC         0xFC   /**< Synchronization */
#define ADS1256_CMD_STANDBY      0xFD   /**< Enter standby mode */
#define ADS1256_CMD_RESET        0xFE   /**< Reset */

/* Operating modes */
#define ADS1256_MODE_NORMAL       0     /**< Normal mode */
#define ADS1256_MODE_DUTY_CYCLE   1     /**< Duty-cycle mode */
#define ADS1256_MODE_TURBO        2     /**< Turbo mode */

/* Conversion modes */
#define ADS1256_CONV_SINGLE_SHOT  0     /**< Single-shot conversion */
#define ADS1256_CONV_CONTINUOUS   1     /**< Continuous conversion */

/* Buffer */
#define ADS1256_BUFFER_DISABLED   0     /**< Buffer disabled */
#define ADS1256_BUFFER_ENABLED    1     /**< Buffer enabled */

/* Constants for data rates in Hz */
extern const float ADS1256_SPS_VALUES[16];

/* Data rate register values */
extern const uint8_t ADS1256_DRATE_REGISTER_VALUES[16];

/* Gain register values */
extern const uint8_t ADS1256_GAIN_REGISTER_VALUES[7];

/* Calibration time for different data rates [μs] */
extern const int ADS1256_SELF_CALIBRATION_TIMING[16];

/* Offset calibration time for different data rates [μs] */
extern const int ADS1256_OFFSET_CALIBRATION_TIMING[16];

/* Data rate names for display */
extern const char *ADS1256_DRATE_NAMES[16];

/* Data rates */
typedef enum {
    ADS1256_DRATE_30000 = 0,    /**< 30000 SPS */
    ADS1256_DRATE_15000,        /**< 15000 SPS */
    ADS1256_DRATE_7500,         /**< 7500 SPS */
    ADS1256_DRATE_3750,         /**< 3750 SPS */
    ADS1256_DRATE_2000,         /**< 2000 SPS */
    ADS1256_DRATE_1000,         /**< 1000 SPS */
    ADS1256_DRATE_500,          /**< 500 SPS */
    ADS1256_DRATE_100,          /**< 100 SPS */
    ADS1256_DRATE_60,           /**< 60 SPS */
    ADS1256_DRATE_50,           /**< 50 SPS */
    ADS1256_DRATE_30,           /**< 30 SPS */
    ADS1256_DRATE_25,           /**< 25 SPS */
    ADS1256_DRATE_15,           /**< 15 SPS */
    ADS1256_DRATE_10,           /**< 10 SPS */
    ADS1256_DRATE_5,            /**< 5 SPS */
    ADS1256_DRATE_2_5           /**< 2.5 SPS */
} ads1256_drate_t;

/* Gain */
typedef enum {
    ADS1256_GAIN_1 = 1,         /**< Gain 1x */
    ADS1256_GAIN_2 = 2,         /**< Gain 2x */
    ADS1256_GAIN_4 = 4,         /**< Gain 4x */
    ADS1256_GAIN_8 = 8,         /**< Gain 8x */
    ADS1256_GAIN_16 = 16,       /**< Gain 16x */
    ADS1256_GAIN_32 = 32,       /**< Gain 32x */
    ADS1256_GAIN_64 = 64        /**< Gain 64x */
} ads1256_gain_t;

/* Channels */
typedef enum {
    ADS1256_CHAN_0 = 1,  /**< +AIN0, -AIN1 */
    ADS1256_CHAN_1 = 2,  /**< +AIN2, -AIN3 */
    ADS1256_CHAN_2 = 3,  /**< +AIN4, -AIN5 */
    ADS1256_CHAN_3 = 4   /**< +AIN6, -AIN7 */
} ads1256_chan_t;

/* ADS1256 Configuration */
typedef struct {
    double v_ref;              /**< Reference voltage [V] */
    uint8_t operating_mode;    /**< Operating mode */
    uint8_t conversion_mode;   /**< Conversion mode */
    ads1256_gain_t gain;       /**< Gain */
    ads1256_chan_t channel;    /**< Active channel */
    ads1256_drate_t drate;     /**< Data rate */
    uint8_t buffer_enabled;    /**< Buffer status */
    uint32_t drdy_timeout_ms;  /**< DRDY timeout in ms */
    uint8_t verbose;           /**< Verbose mode */
} ads1256_config_t;

/**
 * @brief Initialize ADS1256 with default configuration
 * 
 * @param fd File descriptor of SPI device
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_init(int fd);

/**
 * @brief Initialize ADS1256 with custom configuration
 * 
 * @param fd File descriptor of SPI device
 * @param config Pointer to configuration structure
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or NULL config
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_init_with_config(int fd, const ads1256_config_t *config);

/**
 * @brief Set operating mode
 * 
 * @param fd File descriptor of SPI device
 * @param mode Operating mode (0-Normal, 1-Duty-cycle, 2-Turbo)
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or mode value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_operating_mode(int fd, uint8_t mode);

/**
 * @brief Set conversion mode
 * 
 * @param fd File descriptor of SPI device
 * @param mode Conversion mode (0-Single shot, 1-Continuous)
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or mode value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_conversion_mode(int fd, uint8_t mode);

/**
 * @brief Set channel
 * 
 * @param fd File descriptor of SPI device
 * @param channel Channel number (ADS1256_CHAN_0 to ADS1256_CHAN_3)
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or channel value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_channel(int fd, int channel);

/**
 * @brief Set gain
 * 
 * @param fd File descriptor of SPI device
 * @param gain Gain (1, 2, 4, 8, 16, 32, 64 or ADS1256_GAIN_x)
 * @return int Current gain on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or gain value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_gain(int fd, int gain);

/**
 * @brief Set buffer
 * 
 * @param fd File descriptor of SPI device
 * @param enable 0 to disable, 1 to enable
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or enable value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_buffer(int fd, uint8_t enable);

/**
 * @brief Set data rate
 * 
 * @param fd File descriptor of SPI device
 * @param drate Data rate index (0-15 or ADS1256_DRATE_x)
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or drate value
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_set_drate(int fd, uint8_t drate);

/**
 * @brief Single voltage measurement
 * 
 * @param fd File descriptor of SPI device
 * @param voltage Pointer to variable to store the result
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor or NULL voltage pointer
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 *   - ADS1256_ERROR_TIMEOUT: Timeout waiting for DRDY signal
 */
int ads1256_read_voltage(int fd, double *voltage);

/**
 * @brief Sample for a specified time
 * 
 * @param fd File descriptor of SPI device
 * @param duration_ms Sampling duration in ms
 * @param samples Pointer to array for storing samples
 * @param max_samples Maximum number of samples that can be stored in samples
 * @param actual_samples Pointer to store the actual number of samples obtained
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 *   - ADS1256_ERROR_MEMORY: Failed to allocate memory for samples
 *   - ADS1256_ERROR_TIMEOUT: Timeout waiting for DRDY signal
 */
int ads1256_sample(int fd, int duration_ms, double *samples, int max_samples, int *actual_samples);

/**
 * @brief Send command to ADS1256
 * 
 * @param fd File descriptor of SPI device
 * @param command Command to send
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_send_command(int fd, uint8_t command);

/**
 * @brief Read register
 * 
 * @param fd File descriptor of SPI device
 * @param reg_addr Register address
 * @param reg_value Pointer to store register value
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_read_register(int fd, uint8_t reg_addr, uint8_t *reg_value);

/**
 * @brief Write to register
 * 
 * @param fd File descriptor of SPI device
 * @param reg_addr Register address
 * @param reg_value Value to write
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 *   - ADS1256_ERROR_COMMUNICATION: Failed to communicate with the device
 */
int ads1256_write_register(int fd, uint8_t reg_addr, uint8_t reg_value);

/**
 * @brief Print values of all registers
 * 
 * @param fd File descriptor of SPI device
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid file descriptor
 *   - ADS1256_ERROR_COMMUNICATION: Failed to read registers
 */
int ads1256_dump_registers(int fd);

/**
 * @brief Set timeout for waiting for DRDY signal
 * 
 * @param fd File descriptor of SPI device
 * @param timeout_ms Timeout in milliseconds
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 */
int ads1256_set_drdy_timeout(int fd, uint32_t timeout_ms);

/**
 * @brief Get current ADS1256 configuration
 * 
 * @param fd File descriptor of SPI device
 * @param config Pointer to structure to store configuration
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 */
int ads1256_get_config(int fd, ads1256_config_t *config);

/**
 * @brief Set verbose mode (output to stdout)
 * 
 * @param fd File descriptor of SPI device
 * @param verbose 0 to disable, 1 to enable
 * @return int ADS1256_OK on success, negative error code on failure
 * 
 * @note Possible errors:
 *   - ADS1256_ERROR_PARAMETER: Invalid parameters
 */
int ads1256_set_verbose(int fd, uint8_t verbose);

/**
 * @brief Get error message text
 * 
 * @param error_code Error code
 * @return const char* Text description of the error
 */
const char* ads1256_strerror(int error_code);

#endif /* ADS1256_LIB_H */
