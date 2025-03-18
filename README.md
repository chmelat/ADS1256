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
- **Multi-Device Support**: Designed to manage multiple ADS1256 devices simultaneously
- **Optimized Register Operations**: Efficient bit manipulation for register updates
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
DRDY      GPIO  (Pin 7)
```

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
result = ads1256_init_with_config(fd, &config);
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

- **Initialization and Configuration**
  - `ads1256_init()` - Initialize with default settings
  - `ads1256_init_with_config()` - Initialize with custom settings
  - `ads1256_set_operating_mode()` - Set operating mode
  - `ads1256_set_conversion_mode()` - Set conversion mode
  - `ads1256_set_channel()` - Select input channel
  - `ads1256_set_gain()` - Set amplifier gain
  - `ads1256_set_buffer()` - Enable/disable input buffer
  - `ads1256_set_drate()` - Set data rate

- **Data Acquisition**
  - `ads1256_read_voltage()` - Read single voltage measurement
  - `ads1256_sample()` - Perform continuous sampling

- **Management and Control**
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

## Advanced Example: High-Speed Data Acquisition

```c
// High-speed configuration
ads1256_config_t config = {
    .v_ref = 2.037,
    .operating_mode = ADS1256_MODE_TURBO,       // Turbo mode
    .drate = ADS1256_DRATE_30000,               // 30,000 SPS
    .conversion_mode = ADS1256_CONV_CONTINUOUS, // Continuous conversion
    .buffer_enabled = ADS1256_BUFFER_DISABLED,  // Disable buffer for speed
    .drdy_timeout_ms = 1000                     // 1 second timeout
};

ads1256_init_with_config(fd, &config);

// Allocate memory and sample for 100ms
double *samples = malloc(3000 * sizeof(double)); // 30kSPS * 0.1s = 3000 samples
int actual_samples;

if (ads1256_sample(fd, 100, samples, 3000, &actual_samples) == ADS1256_OK) {
    printf("Captured %d samples at 30 kSPS\n", actual_samples);
    // Process the data...
}

free(samples);
```

## Parameter Validation

In version 3.1, we've enhanced parameter validation using macros that provide clear error codes:

```c
// These are used internally in the library to validate parameters
#define CHECK_NULL_PARAM(param) if ((param) == NULL) return ADS1256_ERROR_PARAMETER
#define CHECK_RANGE_PARAM(param, min, max) if ((param) < (min) || (param) > (max)) return ADS1256_ERROR_PARAMETER
```

Each function validates its inputs to prevent runtime errors, making the library more robust.

## Changes in Version 3.3 (2025-03-18)

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

## License

This library is distributed under the MIT License.

## Acknowledgments

The library is based on the original ADS1256 code and has been substantially enhanced to improve robustness, usability, and error handling.
