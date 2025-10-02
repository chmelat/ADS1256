/**
 * @file ads1256_lib.c
 * @brief Implementation of optimized library for ADS1256 24-bit ADC
 * @version 3.5
 * @date 2025-10-02
 * 
 * Changes in v3.5:
 * - Fixed timing using clock_gettime instead of clock()
 * - Added proper error checking for SPI operations
 * - Replaced magic numbers with named constants
 * - Improved parameter validation
 * - Better error handling consistency
 * 
 * Changes in v3.4:
 * - Context management now uses proper fd lookup instead of array indexing
 * - Added NULL checks after all get_context() calls
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <linux/spi/spidev.h>

#include "spi_base.h"
#include "ads1256_lib.h"

/* Constants for data rates in Hz */
const float ADS1256_SPS_VALUES[16] = {
    30000.0f, 15000.0f, 7500.0f, 3750.0f, 2000.0f, 1000.0f, 500.0f, 100.0f, 
    60.0f, 50.0f, 30.0f, 25.0f, 15.0f, 10.0f, 5.0f, 2.5f
};

/* Data rate register values */
const uint8_t ADS1256_DRATE_REGISTER_VALUES[16] = {
    0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA1, 0x92, 0x82, 
    0x72, 0x63, 0x53, 0x43, 0x33, 0x23, 0x13, 0x03
};

/* Gain register values */
const uint8_t ADS1256_GAIN_REGISTER_VALUES[7] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06  /* 1x, 2x, 4x, 8x, 16x, 32x, 64x */
};

/* Calibration time for different data rates [μs] */
const int ADS1256_SELF_CALIBRATION_TIMING[16] = {
    892, 896, 1029, 1300, 2000, 3600, 6600, 31200,
    50900, 61800, 101300, 123200, 202100, 307200, 613800, 1227200
};

/* Offset calibration time for different data rates [μs] */
const int ADS1256_OFFSET_CALIBRATION_TIMING[16] = {
    387, 453, 587, 853, 1300, 2300, 4300, 20300,
    33700, 40300, 67000, 80300, 133700, 200300, 400300, 800300
};

/* Data rate names for display */
const char *ADS1256_DRATE_NAMES[16] = {
    "30000", "15000", "7500", "3750", "2000", "1000", "500", "100",
    "60", "50", "30", "25", "15", "10", "5", "2.5"
};

/* Structure to store context for each ADS1256 */
typedef struct {
    double v_ref;                 /* Reference voltage [V] */
    uint8_t reg_conf[11];         /* Configuration registers */
    uint8_t reg_data[3];          /* Data register */
    int gain;                     /* Gain (1, 2, 4, 8, 16, 32, 64) */
    int channel;                  /* Selected channel (1-4) */
    uint8_t drate;                /* Data rate index (0-15) */
    uint8_t operating_mode;       /* Operating mode */
    uint8_t conversion_mode;      /* Conversion mode */
    uint8_t buffer_enabled;       /* Buffer status */
    uint32_t drdy_timeout_ms;     /* Timeout for DRDY in ms */
    uint8_t verbose;              /* Output messages */
} ads1256_context_t;

/* Device slot structure for proper fd management */
typedef struct {
    int fd;                       /* File descriptor */
    ads1256_context_t context;    /* Device context */
    int active;                   /* Slot in use flag */
} device_slot_t;

/* Error messages */
static const char *error_messages[] = {
    "Success",
    "Invalid parameter",
    "Communication error",
    "Memory allocation error",
    "Timeout expired"
};

/* Global device slots array */
#define MAX_SPI_DEVICES 8
static device_slot_t device_slots[MAX_SPI_DEVICES] = {0};

/* Timing constants */
#define DRDY_POLL_INTERVAL_US 100
#define RESET_DELAY_US 10000
#define SYNC_DELAY_US 4


/**
 * Find context slot by file descriptor
 * @return Pointer to context or NULL if not found
 */
static ads1256_context_t* get_context(int fd)
{
    for (int i = 0; i < MAX_SPI_DEVICES; i++) {
        if (device_slots[i].active && device_slots[i].fd == fd) {
            return &device_slots[i].context;
        }
    }
    return NULL;
}

/**
 * Find free slot for new device
 * @return Slot index or -1 if all slots are occupied
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_SPI_DEVICES; i++) {
        if (!device_slots[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * Register new device in a slot
 * @return Slot index or -1 on error
 */
static int register_device(int fd)
{
    /* Check if already registered */
    for (int i = 0; i < MAX_SPI_DEVICES; i++) {
        if (device_slots[i].active && device_slots[i].fd == fd) {
            return i;  /* Already registered */
        }
    }
    
    /* Find free slot */
    int slot = find_free_slot();
    if (slot < 0) {
        return -1;  /* No free slots */
    }
    
    /* Initialize slot */
    memset(&device_slots[slot], 0, sizeof(device_slot_t));
    device_slots[slot].fd = fd;
    device_slots[slot].active = 1;
    
    return slot;
}

/**
 * Unregister device from slot
 */
static void unregister_device(int fd)
{
    for (int i = 0; i < MAX_SPI_DEVICES; i++) {
        if (device_slots[i].active && device_slots[i].fd == fd) {
            device_slots[i].active = 0;
            device_slots[i].fd = -1;
            memset(&device_slots[i].context, 0, sizeof(ads1256_context_t));
            break;
        }
    }
}

/**
 * Helper function to update bits in a register
 */
static int update_register_bits(int fd, uint8_t reg_addr, uint8_t mask, uint8_t value)
{
    uint8_t reg_value;
    int result = ads1256_read_register(fd, reg_addr, &reg_value);
    if (result != ADS1256_OK) {
        return result;
    }
    
    reg_value = (reg_value & ~mask) | (value & mask);
    return ads1256_write_register(fd, reg_addr, reg_value);
}

/**
 * Wait for data ready (DRDY) - read from STATUS register
 * Uses clock_gettime for accurate wall-clock timing
 */
static int wait_for_drdy(int fd)
{
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }

    uint8_t status;
    struct timespec start_time, current_time;
    
    /* Get start time using monotonic clock */
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    /* Calculate timeout in nanoseconds to avoid overflow */
    uint64_t timeout_ns = (uint64_t)ctx->drdy_timeout_ms * 1000000ULL;

    do {
        int result = ads1256_read_register(fd, ADS1256_REG_STATUS, &status);
        if (result != ADS1256_OK) {
            return result;
        }
        
        /* Check DRDY bit (bit 0) - when 0, data is ready */
        if (!(status & ADS1256_STATUS_DRDY_MASK)) {
            return ADS1256_OK;
        }
        
        usleep(DRDY_POLL_INTERVAL_US);

        /* Check timeout */
        if (clock_gettime(CLOCK_MONOTONIC, &current_time) != 0) {
            return ADS1256_ERROR_COMMUNICATION;
        }
        
        /* Calculate elapsed time in nanoseconds */
        uint64_t elapsed_ns = (uint64_t)(current_time.tv_sec - start_time.tv_sec) * 1000000000ULL +
                              (uint64_t)(current_time.tv_nsec - start_time.tv_nsec);
        
        if (elapsed_ns > timeout_ns) {
            return ADS1256_ERROR_TIMEOUT;
        }
    } while (1);
}

/**
 * Print command information in verbose mode
 */
static void print_command_info(uint8_t command)
{
    const char *cmd_name = "Unknown command";
    
    switch (command) {
        case ADS1256_CMD_WAKEUP:   cmd_name = "Wake-up"; break;
        case ADS1256_CMD_RDATA:    cmd_name = "Read data"; break;
        case ADS1256_CMD_RDATAC:   cmd_name = "Read data continuously"; break;
        case ADS1256_CMD_SDATAC:   cmd_name = "Stop read data continuously"; break;
        case ADS1256_CMD_RREG:     cmd_name = "Read register"; break;
        case ADS1256_CMD_WREG:     cmd_name = "Write register"; break;
        case ADS1256_CMD_SELFCAL:  cmd_name = "Self calibration"; break;
        case ADS1256_CMD_SELFOCAL: cmd_name = "Self offset calibration"; break;
        case ADS1256_CMD_SELFGCAL: cmd_name = "Self gain calibration"; break;
        case ADS1256_CMD_SYSOCAL:  cmd_name = "System offset calibration"; break;
        case ADS1256_CMD_SYSGCAL:  cmd_name = "System gain calibration"; break;
        case ADS1256_CMD_SYNC:     cmd_name = "Sync"; break;
        case ADS1256_CMD_STANDBY:  cmd_name = "Standby"; break;
        case ADS1256_CMD_RESET:    cmd_name = "Reset"; break;
    }
    
    printf("%s ... ", cmd_name);
}

/**
 * Convert 24-bit value to voltage
 */
static double raw_to_voltage(ads1256_context_t *ctx, int32_t raw_value)
{
    /* Validate gain to prevent division by zero */
    if (ctx->gain == 0) {
        return 0.0;
    }
    
    /* Convert 24-bit signed value */
    if (raw_value & 0x800000) {
        raw_value |= ~0xFFFFFF;  /* Extend sign bit */
    }
    
    /* Calculate full scale and convert to voltage */
    double full_scale = 2.0 * ctx->v_ref / ctx->gain;
    return full_scale * raw_value / 0x800000;
}

/**
 * Get 24-bit value from array of three bytes
 */
static int32_t get_24bit_value(const uint8_t *data)
{
    return ((int32_t)data[0] << 16) | ((int32_t)data[1] << 8) | (int32_t)data[2];
}

/**
 * Read data from ADC
 */
static int read_adc_data(int fd, uint8_t *data)
{
    int result;
    if ((result = wait_for_drdy(fd)) != ADS1256_OK) {
        return result;
    }
    
    tx[0] = ADS1256_CMD_RDATA;
    tx[1] = tx[2] = tx[3] = 0;
    
    result = spi_trans(fd, 4);
    if (result != 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    data[0] = rx[1];
    data[1] = rx[2];
    data[2] = rx[3];
    
    return ADS1256_OK;
}

/* ===== Public API functions ===== */

int ads1256_init(int fd)
{
    if (fd < 0) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    ads1256_config_t default_config = {
        .v_ref = 2.037,               /* Default reference voltage */
        .operating_mode = ADS1256_MODE_NORMAL,
        .conversion_mode = ADS1256_CONV_SINGLE_SHOT,
        .gain = ADS1256_GAIN_1,
        .channel = ADS1256_CHAN_0,
        .drate = ADS1256_DRATE_100,   /* 100 SPS for good accuracy */
        .buffer_enabled = ADS1256_BUFFER_DISABLED,
        .drdy_timeout_ms = 5000,      /* 5 seconds timeout */
        .verbose = 0
    };
    
    return ads1256_init_with_config(fd, &default_config);
}

int ads1256_init_with_config(int fd, const ads1256_config_t *config)
{
    if (fd < 0) {
        return ADS1256_ERROR_PARAMETER;
    }
    CHECK_NULL_PARAM(config);
    
    /* Validate configuration values */
    if (config->v_ref < ADS1256_MIN_VREF || config->v_ref > ADS1256_MAX_VREF) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (config->drdy_timeout_ms < ADS1256_MIN_TIMEOUT_MS) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (config->operating_mode > ADS1256_MODE_TURBO) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (config->conversion_mode > ADS1256_CONV_CONTINUOUS) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (config->drate > ADS1256_DRATE_2_5) {
        return ADS1256_ERROR_PARAMETER;
    }
    if (config->buffer_enabled > ADS1256_BUFFER_ENABLED) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Register device and get slot */
    int slot = register_device(fd);
    if (slot < 0) {
        return ADS1256_ERROR_MEMORY;  /* All slots occupied */
    }
    
    ads1256_context_t *ctx = &device_slots[slot].context;
    
    /* Set configuration values */
    ctx->v_ref = config->v_ref;
    ctx->gain = config->gain;
    ctx->channel = config->channel;
    ctx->drate = config->drate;
    ctx->operating_mode = config->operating_mode;
    ctx->conversion_mode = config->conversion_mode;
    ctx->buffer_enabled = config->buffer_enabled;
    ctx->drdy_timeout_ms = config->drdy_timeout_ms;
    ctx->verbose = config->verbose;
    
    /* Reset ADC */
    int result;
    if ((result = ads1256_send_command(fd, ADS1256_CMD_RESET)) != ADS1256_OK) {
        unregister_device(fd);
        return result;
    }
    usleep(RESET_DELAY_US);

    /* Configure basic parameters */
    if ((result = ads1256_set_drate(fd, ctx->drate)) != ADS1256_OK ||
        (result = ads1256_set_gain(fd, ctx->gain)) != ADS1256_OK ||
        (result = ads1256_set_buffer(fd, ctx->buffer_enabled)) != ADS1256_OK ||
        (result = ads1256_set_channel(fd, ctx->channel)) != ADS1256_OK ||
        (result = ads1256_set_operating_mode(fd, ctx->operating_mode)) != ADS1256_OK ||
        (result = ads1256_set_conversion_mode(fd, ctx->conversion_mode)) != ADS1256_OK) {
        unregister_device(fd);
        return result;
    }
    
    /* Perform self-calibration */
    if ((result = ads1256_send_command(fd, ADS1256_CMD_SELFCAL)) != ADS1256_OK) {
        unregister_device(fd);
        return result;
    }
    
    usleep(ADS1256_SELF_CALIBRATION_TIMING[ctx->drate]);
    
    if (ctx->verbose) {
        printf("ADS1256 initialized with:\n");
        printf("  Reference voltage: %.3f V\n", ctx->v_ref);
        printf("  Gain: %d\n", ctx->gain);
        printf("  Channel: %d\n", ctx->channel);
        printf("  Data rate: %s SPS\n", ADS1256_DRATE_NAMES[ctx->drate]);
        printf("  Operating mode: %d\n", ctx->operating_mode);
        printf("  Conversion mode: %d\n", ctx->conversion_mode);
        printf("  Buffer: %s\n", ctx->buffer_enabled ? "Enabled" : "Disabled");
    }
    
    return ADS1256_OK;
}

void ads1256_cleanup(int fd)
{
    unregister_device(fd);
}

int ads1256_read_register(int fd, uint8_t reg_addr, uint8_t *reg_value)
{
    CHECK_RANGE_PARAM(reg_addr, 0, 0x0A);
    CHECK_NULL_PARAM(reg_value);
    
    tx[0] = ADS1256_CMD_RREG | (reg_addr & 0x0F);
    tx[1] = 0x00;  /* Read one register */
    tx[2] = 0x00;  /* For receiving data */
    
    int result = spi_trans(fd, 3);
    if (result != 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    *reg_value = rx[2];
    
    /* Save value to context if available */
    ads1256_context_t *ctx = get_context(fd);
    if (ctx) {
        ctx->reg_conf[reg_addr] = *reg_value;
    }
    
    return ADS1256_OK;
}

int ads1256_write_register(int fd, uint8_t reg_addr, uint8_t reg_value)
{
    CHECK_RANGE_PARAM(reg_addr, 0, 0x0A);
    
    ads1256_context_t *ctx = get_context(fd);
    if (ctx) {
        ctx->reg_conf[reg_addr] = reg_value;
    }
    
    tx[0] = ADS1256_CMD_WREG | (reg_addr & 0x0F);
    tx[1] = 0x00;  /* Write one register */
    tx[2] = reg_value;
    
    int result = spi_trans(fd, 3);
    if (result != 0) {
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    return ADS1256_OK;
}

int ads1256_set_operating_mode(int fd, uint8_t mode)
{
    CHECK_RANGE_PARAM(mode, 0, 2);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Set operation mode bits in MUX register (bits 3-4) */
    uint8_t mode_bits;
    switch (mode) {
        case ADS1256_MODE_NORMAL:
            mode_bits = 0x00;  /* Bits 3-4 cleared */
            break;
        case ADS1256_MODE_DUTY_CYCLE:
            mode_bits = 0x08;  /* Bit 3 set */
            break;
        case ADS1256_MODE_TURBO:
            mode_bits = 0x10;  /* Bit 4 set */
            break;
        default:
            return ADS1256_ERROR_PARAMETER;
    }
    
    int result = update_register_bits(fd, ADS1256_REG_MUX, ADS1256_MUX_MODE_MASK, mode_bits);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->operating_mode = mode;
    
    if (ctx->verbose) {
        printf("Operating mode set to ");
        switch (mode) {
            case ADS1256_MODE_NORMAL:     printf("Normal"); break;
            case ADS1256_MODE_DUTY_CYCLE: printf("Duty-cycle"); break;
            case ADS1256_MODE_TURBO:      printf("Turbo"); break;
        }
        printf(" ... ok\n");
    }
    
    return ADS1256_OK;
}

int ads1256_set_conversion_mode(int fd, uint8_t mode)
{
    CHECK_RANGE_PARAM(mode, 0, 1);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Set/clear continuous conversion bit (bit 1) in MUX register */
    uint8_t mode_bit = (mode == ADS1256_CONV_CONTINUOUS) ? ADS1256_MUX_CONV_MODE_MASK : 0x00;
    
    int result = update_register_bits(fd, ADS1256_REG_MUX, ADS1256_MUX_CONV_MODE_MASK, mode_bit);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->conversion_mode = mode;
    
    if (ctx->verbose) {
        printf("Conversion mode set to %s ... ok\n", 
               mode == ADS1256_CONV_SINGLE_SHOT ? "Single-shot" : "Continuous");
    }
    
    return ADS1256_OK;
}

int ads1256_set_channel(int fd, int channel)
{
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    uint8_t mux_value = 0;
    
    switch (channel) {
        case ADS1256_CHAN_0:  /* +AIN0, -AIN1 */
            mux_value = 0x01;
            break;
        case ADS1256_CHAN_1:  /* +AIN2, -AIN3 */
            mux_value = 0x23;
            break;
        case ADS1256_CHAN_2:  /* +AIN4, -AIN5 */
            mux_value = 0x45;
            break;
        case ADS1256_CHAN_3:  /* +AIN6, -AIN7 */
            mux_value = 0x67;
            break;
        default:
            return ADS1256_ERROR_PARAMETER;
    }
    
    /* Preserve operating mode and conversion mode bits */
    uint8_t current_value;
    int result = ads1256_read_register(fd, ADS1256_REG_MUX, &current_value);
    if (result != ADS1256_OK) {
        return result;
    }
    
    /* Clear input selection bits but keep mode bits (3-4) and conversion bit (1) */
    mux_value |= (current_value & (ADS1256_MUX_MODE_MASK | ADS1256_MUX_CONV_MODE_MASK));
    
    result = ads1256_write_register(fd, ADS1256_REG_MUX, mux_value);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->channel = channel;
    
    if (ctx->verbose) {
        printf("Channel set to %d ... ok\n", channel);
    }
    
    return ADS1256_OK;
}

int ads1256_set_gain(int fd, int gain)
{
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Check valid gain */
    int gain_idx;
    switch (gain) {
        case ADS1256_GAIN_1:  gain_idx = 0; break;
        case ADS1256_GAIN_2:  gain_idx = 1; break;
        case ADS1256_GAIN_4:  gain_idx = 2; break;
        case ADS1256_GAIN_8:  gain_idx = 3; break;
        case ADS1256_GAIN_16: gain_idx = 4; break;
        case ADS1256_GAIN_32: gain_idx = 5; break;
        case ADS1256_GAIN_64: gain_idx = 6; break;
        default:
            return ADS1256_ERROR_PARAMETER;
    }
    
    /* Set gain bits in ADCON register (bits 0-2) */
    int result = update_register_bits(fd, ADS1256_REG_ADCON, ADS1256_ADCON_GAIN_MASK, 
                                      ADS1256_GAIN_REGISTER_VALUES[gain_idx]);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->gain = gain;
    
    if (ctx->verbose) {
        printf("Gain set to %d ... ok\n", gain);
    }
    
    return ADS1256_OK;
}

int ads1256_set_buffer(int fd, uint8_t enable)
{
    CHECK_RANGE_PARAM(enable, 0, 1);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Set/clear buffer bit (bit 1) in STATUS register */
    uint8_t buffer_bit = enable ? ADS1256_STATUS_BUFFER_MASK : 0x00;
    
    int result = update_register_bits(fd, ADS1256_REG_STATUS, ADS1256_STATUS_BUFFER_MASK, buffer_bit);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->buffer_enabled = enable;
    
    if (ctx->verbose) {
        printf("Buffer %s ... ok\n", enable ? "enabled" : "disabled");
    }
    
    return ADS1256_OK;
}

int ads1256_set_drate(int fd, uint8_t drate)
{
    CHECK_RANGE_PARAM(drate, 0, 15);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    int result = ads1256_write_register(fd, ADS1256_REG_DRATE, ADS1256_DRATE_REGISTER_VALUES[drate]);
    if (result != ADS1256_OK) {
        return result;
    }
    
    ctx->drate = drate;
    
    if (ctx->verbose) {
        printf("Data rate set to %s SPS ... ok\n", ADS1256_DRATE_NAMES[drate]);
    }
    
    return ADS1256_OK;
}

int ads1256_read_voltage(int fd, double *voltage)
{
    CHECK_NULL_PARAM(voltage);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    int result = read_adc_data(fd, ctx->reg_data);
    if (result != ADS1256_OK) {
        return result;
    }
    
    int32_t raw_value = get_24bit_value(ctx->reg_data);
    *voltage = raw_to_voltage(ctx, raw_value);
    
    return ADS1256_OK;
}

int ads1256_sample(int fd, int duration_ms, double *samples, int max_samples, int *actual_samples)
{
    CHECK_RANGE_PARAM(duration_ms, 1, INT_MAX);
    CHECK_NULL_PARAM(samples);
    CHECK_RANGE_PARAM(max_samples, 1, INT_MAX);
    CHECK_NULL_PARAM(actual_samples);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    /* Calculate number of samples based on data rate */
    float samples_per_sec = ADS1256_SPS_VALUES[ctx->drate];
    
    /* Check for potential overflow before calculation */
    if (samples_per_sec > (float)INT_MAX * 1000.0f / duration_ms) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    int expected_samples = (int)(samples_per_sec * duration_ms / 1000.0f);
    int num_samples = (expected_samples < max_samples) ? expected_samples : max_samples;
    
    if (num_samples == 0) {
        *actual_samples = 0;
        return ADS1256_OK;  /* Too short duration or too low data rate */
    }
    
    /* Allocate memory for raw data */
    uint8_t *raw_data = (uint8_t*)malloc((size_t)num_samples * 3);
    if (!raw_data) {
        return ADS1256_ERROR_MEMORY;
    }
    
    /* Prepare SPI transfer for continuous reading */
    tx[0] = ADS1256_CMD_RDATAC;
    int result = spi_write(fd, tx, 1);
    if (result != 0) {
        free(raw_data);
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    /* Read samples */
    size_t samples_read = 0;
    for (int i = 0; i < num_samples; i++) {
        if (wait_for_drdy(fd) != ADS1256_OK) {
            /* Cleanup in case of error - attempt to stop continuous reading */
            tx[0] = ADS1256_CMD_SDATAC;
            spi_write(fd, tx, 1);  /* Ignore error during cleanup */
            free(raw_data);
            *actual_samples = (int)samples_read;
            return ADS1256_ERROR_TIMEOUT;
        }
        
        result = spi_read(fd, &raw_data[i * 3], 3);
        if (result != 0) {
            /* Cleanup on error */
            tx[0] = ADS1256_CMD_SDATAC;
            spi_write(fd, tx, 1);  /* Ignore error during cleanup */
            free(raw_data);
            *actual_samples = (int)samples_read;
            return ADS1256_ERROR_COMMUNICATION;
        }
        samples_read++;
    }
    
    /* Stop continuous reading */
    tx[0] = ADS1256_CMD_SDATAC;
    result = spi_write(fd, tx, 1);
    if (result != 0) {
        free(raw_data);
        *actual_samples = (int)samples_read;
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    /* Convert raw data to voltage */
    for (size_t i = 0; i < samples_read; i++) {
        int32_t raw_value = get_24bit_value(&raw_data[i * 3]);
        samples[i] = raw_to_voltage(ctx, raw_value);
    }
    
    free(raw_data);
    *actual_samples = (int)samples_read;
    
    return ADS1256_OK;
}

int ads1256_send_command(int fd, uint8_t command)
{
    ads1256_context_t *ctx = get_context(fd);
    
    if (ctx && ctx->verbose) {
        print_command_info(command);
    }
    
    tx[0] = command;
    int result = spi_write(fd, tx, 1);
    if (result != 0) {
        if (ctx && ctx->verbose) {
            printf("failed\n");
        }
        return ADS1256_ERROR_COMMUNICATION;
    }
    
    /* Special processing for some commands */
    switch (command) {
        case ADS1256_CMD_SYNC:
            usleep(SYNC_DELAY_US);
            break;
            
        case ADS1256_CMD_SELFCAL:
        case ADS1256_CMD_SELFOCAL:
        case ADS1256_CMD_SELFGCAL:
        case ADS1256_CMD_SYSOCAL:
        case ADS1256_CMD_SYSGCAL:
            /* Delay for calibration based on data rate */
            if (ctx) {
                if (command == ADS1256_CMD_SELFCAL) {
                    usleep(ADS1256_SELF_CALIBRATION_TIMING[ctx->drate]);
                } else if (command == ADS1256_CMD_SELFOCAL || command == ADS1256_CMD_SYSOCAL) {
                    usleep(ADS1256_OFFSET_CALIBRATION_TIMING[ctx->drate]);
                }
            }
            break;
            
        default:
            /* For other commands wait for DRDY */
            result = wait_for_drdy(fd);
            if (result != ADS1256_OK) {
                if (ctx && ctx->verbose) {
                    printf("timeout\n");
                }
                return result;
            }
            break;
    }
    
    if (ctx && ctx->verbose) {
        printf("ok\n");
    }
    
    return ADS1256_OK;
}

int ads1256_dump_registers(int fd)
{
    printf("\nADS1256 Registers:\n");
    printf("--------------------------------------------------\n");
    printf("Address | Value (Bin)         | Value (Hex)\n");
    printf("--------------------------------------------------\n");
    
    for (int i = 0; i <= 0x0A; i++) {
        uint8_t value;
        if (ads1256_read_register(fd, (uint8_t)i, &value) != ADS1256_OK) {
            printf("Error reading register 0x%02X\n", i);
            continue;
        }
        
        printf("0x%02X   | ", i);
        print_binary(value, 8);
        printf(" | 0x%02X\n", value);
    }
    
    printf("--------------------------------------------------\n");
    
    return ADS1256_OK;
}

int ads1256_set_drdy_timeout(int fd, uint32_t timeout_ms)
{
    CHECK_RANGE_PARAM(timeout_ms, ADS1256_MIN_TIMEOUT_MS, UINT32_MAX);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    ctx->drdy_timeout_ms = timeout_ms;
    
    return ADS1256_OK;
}

int ads1256_get_config(int fd, ads1256_config_t *config)
{
    CHECK_NULL_PARAM(config);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    config->v_ref = ctx->v_ref;
    config->operating_mode = ctx->operating_mode;
    config->conversion_mode = ctx->conversion_mode;
    config->gain = (ads1256_gain_t)ctx->gain;
    config->channel = (ads1256_chan_t)ctx->channel;
    config->drate = (ads1256_drate_t)ctx->drate;
    config->buffer_enabled = ctx->buffer_enabled;
    config->drdy_timeout_ms = ctx->drdy_timeout_ms;
    config->verbose = ctx->verbose;
    
    return ADS1256_OK;
}

int ads1256_set_verbose(int fd, uint8_t verbose)
{
    CHECK_RANGE_PARAM(verbose, 0, 1);
    
    ads1256_context_t *ctx = get_context(fd);
    if (!ctx) {
        return ADS1256_ERROR_PARAMETER;
    }
    
    ctx->verbose = verbose;
    
    return ADS1256_OK;
}

const char* ads1256_strerror(int error_code)
{
    if (error_code == ADS1256_OK) {
        return "Success";
    }
    
    error_code = -error_code;  /* Convert to positive index */
    
    if (error_code >= 1 && error_code <= 4) {
        return error_messages[error_code];
    }
    
    return "Unknown error";
}
