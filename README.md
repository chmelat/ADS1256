# ADS1256 Library for Raspberry Pi

## Overview

This library provides a robust C interface for controlling the ADS1256 24-bit analog-to-digital converter via SPI on Raspberry Pi platforms. The ADS1256 is a high-precision, low-noise ADC suitable for applications requiring accurate measurements, such as scientific instrumentation, industrial monitoring, and precision data acquisition systems.

## Features

- **High-Resolution Measurements**: Full 24-bit resolution with configurable gain settings
- **Multiple Channels**: Support for 4 differential input channels
- **Configurable Sampling Rates**: Adjustable from 2.5 SPS to 30,000 SPS
- **Flexible Modes**: Normal, Duty-cycle, and Turbo operating modes
- **Buffer Control**: Optional input buffer to provide high impedance inputs
- **Advanced Error Handling**: Detailed error codes and timeout protection
- **Multi-Device Support**: Designed to manage multiple ADS1256 devices simultaneously (up to 8 devices)
- **Optimized Register Operations**: Efficient bit manipulation for register updates
- **Adaptive Polling**: Intelligent polling intervals based on data rate for optimal performance
- **Accurate Timing**: Uses monotonic clock for precise timeout measurements
- **Comprehensive Parameter Validation**: Robust checks to prevent runtime errors
- **Available as Static Library**: Can be compiled as a standalone static library
- **Detailed Timing Control**: Precise timing constants for calibration operations
- **Extensive Documentation**: Full API documentation with error conditions

## Hardware Connection

Connect the ADS1256 to your Raspberry Pi as follows:

```
ADS1256   Raspberry Pi
-----------------------
CS        CE0   (Pin 24)
DOUT      MISO  (Pin 21)
DIN       MOSI  (Pin 19)
SCLK      SCLK  (Pin 23)
GND       GND   (Any GND pin)
5V        5V    (Pin 2)
DRDY      GPIO  (Pin 7) - optional, not used for polling mode
```

**Note on DRDY**: The library uses register polling to detect data ready status, eliminating the need for GPIO interrupt configuration and root privileges. This provides better portability and easier setup while maintaining good performance through adaptive polling intervals.

## Installation

### Using Makefile

The library can be installed as a static library using the provided Makefile:

```bash
# Build and install the library
make lib
make install

# Or build and install both the library and example program
make install
```

This will:
1. Compile the static library (`libads1256.a`)
2. Install the library to `~/lib`
3. Install the header file to `~/include`
4. Install the example program to `~/bin` (if built)

### Manual Installation

You can also manually include the source files in your project:

```bash
# Copy the necessary files
cp ads1256_lib.h ads1256_lib.c spi_base.h spi_base.c /path/to/your/project
```

## Basic Usage

### Simple Voltage Reading

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "ads1256_lib.h"

int main() {
    int fd = open("/dev/spidev0.0", O_RDWR);
    if (fd < 0) return 1;
    
    // Initialize with default settings
    if (ads1256_init(fd) != ADS1256_OK) {
        close(fd);
        return 1;
    }
    
    // Read voltage
    double voltage;
    if (ads1256_read_voltage(fd, &voltage) == ADS1256_OK) {
        printf("Voltage: %.6f V\n", voltage);
    }
    
    // Cleanup
    ads1256_cleanup(fd);
    close(fd);
    return 0;
}
```

### Custom Configuration

```c
// Custom configuration
ads1256_config_t config = {
    .v_ref = 2.5,                       // 2.5V reference
    .gain = ADS1256_GAIN_8,             // 8x gain
    .channel = ADS1256_CHAN_0,          // Channel 0 (+AIN0, -AIN1)
    .drate = ADS1256_DRATE_1000,        // 1000 SPS
    .buffer_enabled = ADS1256_BUFFER_ENABLED,
    .operating_mode = ADS1256_MODE_NORMAL,
    .conversion_mode = ADS1256_CONV_SINGLE_SHOT,
    .drdy_timeout_ms = 5000,            // 5 second timeout
    .verbose = 0                        // No verbose output
};

// Initialize with custom configuration
if (ads1256_init_with_config(fd, &config) == ADS1256_OK) {
    printf("ADS1256 initialized successfully\n");
}
```

### Sampling Multiple Channels

```c
// Read each channel in sequence
for (int channel = ADS1256_CHAN_0; channel <= ADS1256_CHAN_3; channel++) {
    ads1256_set_channel(fd, channel);
    usleep(5000);  // Allow settling time
    
    double voltage;
    if (ads1256_read_voltage(fd, &voltage) == ADS1256_OK) {
        printf("Channel %d: %.6f V\n", channel, voltage);
    }
}
```

### Continuous Sampling

```c
// Sample for 500ms at the configured data rate
const int max_samples = 1000;
double samples[max_samples];
int actual_samples;

if (ads1256_sample(fd, 500, samples, max_samples, &actual_samples) == ADS1256_OK) {
    printf("Obtained %d samples\n", actual_samples);
    
    // Process first few samples
    for (int i = 0; i < 5 && i < actual_samples; i++) {
        printf("Sample %d: %.6f V\n", i, samples[i]);
    }
}
```

## Linking with the Static Library

If you've installed the library using the Makefile, you can link it in your projects:

```bash
# Compile your program with the static library
gcc -o my_program my_program.c -I$HOME/include -L$HOME/lib -lads1256 -lspi
```

Example Makefile for your project:

```makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2
INCLUDES = -I$(HOME)/include
LIBS = -L$(HOME)/lib -lads1256 -lspi

my_program: my_program.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)
```

## Error Handling

The library uses consistent error codes for all functions:

```c
#define ADS1256_OK                 0    // Operation successful
#define ADS1256_ERROR_PARAMETER   -1    // Invalid parameter
#define ADS1256_ERROR_COMMUNICATION -2  // Communication error
#define ADS1256_ERROR_MEMORY      -3    // Memory allocation error
#define ADS1256_ERROR_TIMEOUT     -4    // Timeout expired
```

Each function in the library documents the possible error codes it can return. For comprehensive error handling, check the return value of each function call and use the `ads1256_strerror()` function to get a textual representation of the error.

Example:

```c
int result = ads1256_read_voltage(fd, &voltage);
if (result != ADS1256_OK) {
    printf("Error: %s\n", ads1256_strerror(result));
    // Handle the error...
}
```

## API Reference

The library provides the following key functions:

### Initialization and Configuration
- `ads1256_init()` - Initialize with default settings
- `ads1256_init_with_config()` - Initialize with custom settings
- `ads1256_cleanup()` - Cleanup and free device slot
- `ads1256_set_operating_mode()` - Set operating mode
- `ads1256_set_conversion_mode()` - Set conversion mode
- `ads1256_set_channel()` - Select input channel
- `ads1256_set_gain()` - Set amplifier gain
- `ads1256_set_buffer()` - Enable/disable input buffer
- `ads1256_set_drate()` - Set data rate

### Data Acquisition
- `ads1256_read_voltage()` - Read single voltage measurement
- `ads1256_sample()` - Perform continuous sampling

### Management and Control
- `ads1256_send_command()` - Send direct command to ADS1256
- `ads1256_read_register()` - Read register value
- `ads1256_write_register()` - Write register value
- `ads1256_dump_registers()` - Print all register values
- `ads1256_set_drdy_timeout()` - Set data ready timeout
- `ads1256_get_config()` - Get current configuration
- `ads1256_set_verbose()` - Enable/disable verbose output
- `ads1256_strerror()` - Get error message text

## Available Constants

The library exposes several useful constants for advanced configuration:

### Register Bit Masks
```c
#define ADS1256_STATUS_DRDY_MASK     0x01   // DRDY bit in STATUS register
#define ADS1256_STATUS_BUFFER_MASK   0x02   // Buffer enable bit
#define ADS1256_MUX_MODE_MASK        0x18   // Operating mode bits
#define ADS1256_MUX_CONV_MODE_MASK   0x02   // Conversion mode bit
#define ADS1256_ADCON_GAIN_MASK      0x07   // Gain bits in ADCON
```

### Data Rates
```c
// Access actual SPS values
extern const float ADS1256_SPS_VALUES[16];  // From 2.5 SPS to 30000 SPS

// Data rate enum values
typedef enum {
    ADS1256_DRATE_30000 = 0,    // 30000 SPS
    ADS1256_DRATE_15000,        // 15000 SPS
    ADS1256_DRATE_7500,         // 7500 SPS
    ...
    ADS1256_DRATE_2_5           // 2.5 SPS
} ads1256_drate_t;
```

### Timing Constants
```c
// Calibration time for different data rates [μs]
extern const int ADS1256_SELF_CALIBRATION_TIMING[16];

// Offset calibration time for different data rates [μs]
extern const int ADS1256_OFFSET_CALIBRATION_TIMING[16];
```

### Validation Limits
```c
#define ADS1256_MIN_VREF          0.1   // Minimum reference voltage
#define ADS1256_MAX_VREF          10.0  // Maximum reference voltage
#define ADS1256_MIN_TIMEOUT_MS    1     // Minimum DRDY timeout
```

## Performance Optimization

### Adaptive Polling
Version 3.5 introduces intelligent polling intervals that adapt to the configured data rate:

- **≥1000 SPS**: Polls every 20 μs for fast response
- **100-1000 SPS**: Polls every 100 μs for balanced performance
- **<100 SPS**: Polls every 500 μs to reduce CPU and SPI bus usage

**Benefits:**
- 2.5 SPS: ~96% reduction in SPI transactions (from ~4000 to ~160)
- 100 SPS: ~90% reduction in SPI transactions (from ~100 to ~10)
- 1000 SPS: ~70% reduction in SPI transactions (from ~10 to ~3)

The library also sleeps for 70% of the expected conversion time before starting to poll, further reducing unnecessary polling.

### Accurate Timing
The library uses `clock_gettime(CLOCK_MONOTONIC)` for timeout measurements, providing:
- Accurate wall-clock timing independent of CPU usage
- Reliable timeout detection even during I/O operations
- Better performance on multi-core systems

## Advanced Example: High-Speed Data Acquisition

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "ads1256_lib.h"

int main() {
    int fd = open("/dev/spidev0.0", O_RDWR);
    if (fd < 0) {
        perror("Failed to open SPI device");
        return 1;
    }
    
    // High-speed configuration
    ads1256_config_t config = {
        .v_ref = 2.037,
        .operating_mode = ADS1256_MODE_TURBO,       // Turbo mode
        .drate = ADS1256_DRATE_30000,               // 30,000 SPS
        .conversion_mode = ADS1256_CONV_CONTINUOUS, // Continuous conversion
        .buffer_enabled = ADS1256_BUFFER_DISABLED,  // Disable buffer for speed
        .gain = ADS1256_GAIN_1,
        .channel = ADS1256_CHAN_0,
        .drdy_timeout_ms = 1000,                    // 1 second timeout
        .verbose = 0
    };

    if (ads1256_init_with_config(fd, &config) != ADS1256_OK) {
        fprintf(stderr, "Failed to initialize ADS1256\n");
        close(fd);
        return 1;
    }

    // Allocate memory and sample for 100ms
    const int expected_samples = 3000; // 30kSPS * 0.1s = 3000 samples
    double *samples = malloc(expected_samples * sizeof(double));
    if (!samples) {
        fprintf(stderr, "Memory allocation failed\n");
        ads1256_cleanup(fd);
        close(fd);
        return 1;
    }
    
    int actual_samples;
    int result = ads1256_sample(fd, 100, samples, expected_samples, &actual_samples);
    
    if (result == ADS1256_OK) {
        printf("Captured %d samples at 30 kSPS\n", actual_samples);
        
        // Calculate statistics
        double sum = 0.0, min = samples[0], max = samples[0];
        for (int i = 0; i < actual_samples; i++) {
            sum += samples[i];
            if (samples[i] < min) min = samples[i];
            if (samples[i] > max) max = samples[i];
        }
        
        printf("Average: %.6f V\n", sum / actual_samples);
        printf("Min: %.6f V, Max: %.6f V\n", min, max);
        printf("Range: %.6f V\n", max - min);
    } else {
        fprintf(stderr, "Sampling failed: %s\n", ads1256_strerror(result));
    }

    free(samples);
    ads1256_cleanup(fd);
    close(fd);
    return 0;
}
```

## Parameter Validation

The library provides comprehensive parameter validation using helper macros:

```c
// These are used internally in the library to validate parameters
#define CHECK_NULL_PARAM(param) if ((param) == NULL) return ADS1256_ERROR_PARAMETER
#define CHECK_RANGE_PARAM(param, min, max) if ((param) < (min) || (param) > (max)) return ADS1256_ERROR_PARAMETER
```

Each function validates its inputs to prevent runtime errors, including:
- NULL pointer checks
- File descriptor validation
- Reference voltage range (0.1V - 10.0V)
- Data rate index validation (0-15)
- Gain value validation
- Timeout value validation (≥1 ms)
- Register address validation

## Resource Management

**Important:** Always call `ads1256_cleanup()` before closing the file descriptor to properly free the device slot:

```c
int fd = open("/dev/spidev0.0", O_RDWR);
ads1256_init(fd);

// ... use the device ...

ads1256_cleanup(fd);  // Free the device slot
close(fd);            // Close the file descriptor
```

The library supports up to 8 simultaneous ADS1256 devices. Failing to call `ads1256_cleanup()` will leave the device slot occupied until the program exits.

## Version History

### Version 3.5 (2025-10-02)
- **Fixed timing**: Uses `clock_gettime(CLOCK_MONOTONIC)` instead of `clock()` for accurate timeout measurements
- **Adaptive polling**: Intelligent polling intervals based on data rate reduce SPI transactions by up to 96%
- **Named constants**: All magic numbers replaced with descriptive bit mask constants
- **Enhanced validation**: Comprehensive parameter validation including reference voltage and timeout ranges
- **Better error handling**: All SPI operations now check return values and propagate errors correctly
- **Overflow protection**: Added checks to prevent integer overflow in sample calculations
- **Improved cleanup**: Better error handling in cleanup paths during sample operations

### Version 3.4 (2025-10-01)
- Fixed context management (proper fd lookup instead of array indexing)
- Added NULL checks after all get_context() calls
- Added ads1256_cleanup() function for proper resource cleanup

### Version 3.3 (2025-03-18)
- Added optimized register bit manipulation
- Improved parameter validation with helpful macros
- Added support for creating static library
- Enhanced error reporting and documentation
- Optimized wait_for_drdy implementation
- Exposed data rate and timing constants for application use
- Added comprehensive comments and documentation
- Improved multi-device support with context management
- Added verbose mode for debugging and development
- Enhanced calibration timing controls

## Troubleshooting

### Permission Denied on /dev/spidev0.0
Add your user to the `spi` group:
```bash
sudo usermod -a -G spi $USER
# Log out and back in for changes to take effect
```

### Timeout Errors
- Check your wiring connections
- Verify the ADS1256 is powered (5V and GND)
- Increase timeout value: `ads1256_set_drdy_timeout(fd, 10000);`
- Enable verbose mode to see detailed operation logs: `ads1256_set_verbose(fd, 1);`

### Incorrect Readings
- Verify reference voltage setting matches your hardware
- Check gain setting is appropriate for your signal range
- Ensure proper grounding
- Allow settling time after channel switching
- Consider enabling the input buffer for high impedance sources

### SPI Communication Errors
- Verify SPI is enabled: `sudo raspi-config` → Interface Options → SPI
- Check SPI speed is appropriate (default 500 kHz)
- Verify correct SPI mode is set
- Check for loose connections

## License

This library is distributed under the MIT License.

## Contributing

Contributions are welcome! Please ensure that:
- Code follows the existing style and conventions
- All functions include proper error handling
- Documentation is updated for any API changes
- Parameter validation is implemented for new functions

## Acknowledgments

The library is based on the original ADS1256 code and has been substantially enhanced to improve robustness, usability, error handling, and performance.