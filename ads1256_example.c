/* 
 *  ADS1256 Example - Using 24-bit, low-noise ADC with 4 Channels
 *  
 *  Wiring
 *  ADS1256    RPi
 *  ---------------------
 *  CS        CE0   (24)
 *  DOUT      MISO  (21)
 *  DIN       MOSI  (19)
 *  SCLK      SCLK  (23)
 *  GND       GND   (6,9,14,20,25,30,34,39)
 *  5V        5V    (2)
 *  DRDY      GPIO  (7)
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include "spi_base.h"
#include "ads1256_lib.h"

// Function to set up SPI interface
int setup_spi(const char* device, uint32_t mode, uint8_t bits_per_word, uint32_t speed_hz) {
    int fd;
    
    // Open SPI device
    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("Cannot open SPI device");
        return -1;
    }
    
    // Configure SPI
    if (verbose_spi) {
        printf("SPI Configuration:\n");
        printf("  Device: %s\n", device);
        printf("  Mode: %d\n", mode);
        printf("  Bits per word: %d\n", bits_per_word);
        printf("  Speed: %d Hz (%d KHz)\n", speed_hz, speed_hz/1000);
    }
    
    // Set SPI parameters
    set_spi_mode(fd, mode);
    set_spi_bpw(fd, bits_per_word);
    set_spi_speed(fd, speed_hz);
    
    // Set up SPI transfer buffers
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    
    return fd;
}

int main(void) {
    // SPI configuration parameters
    uint32_t mode = 1;          // SPI mode for ADS1256
    uint8_t bits_per_word = 0;  // Default (8 bits)
    uint32_t speed_hz = 256000; // 256 kHz
    const char *device = "/dev/spidev0.0";
    int fd, result;
    
    // Enable verbose output
    verbose_spi = 1;
    
    // Open and configure SPI interface
    fd = setup_spi(device, mode, bits_per_word, speed_hz);
    if (fd < 0) {
        return EXIT_FAILURE;
    }
    
    printf("\n=== ADS1256 Example Program ===\n\n");
    
    // Initialize ADS1256 with default configuration
    result = ads1256_init(fd);
    if (result != ADS1256_OK) {
        printf("Failed to initialize ADS1256: %s\n", ads1256_strerror(result));
        close(fd);
        return EXIT_FAILURE;
    }
    
    // Configure ADC
    ads1256_set_channel(fd, 1);
    ads1256_set_gain(fd, 1);
    ads1256_set_operating_mode(fd, ADS1256_MODE_NORMAL);
    ads1256_set_drate(fd, ADS1256_DRATE_15000);  // 15000 SPS
    ads1256_set_buffer(fd, ADS1256_BUFFER_DISABLED);
    
    // Perform self-calibration
    printf("\nPerforming self-calibration...\n");
    result = ads1256_send_command(fd, ADS1256_CMD_SELFCAL);
    if (result != ADS1256_OK) {
        printf("Calibration failed: %s\n", ads1256_strerror(result));
    }
    
    // Synchronize ADC
    printf("Synchronizing ADC...\n");
    ads1256_send_command(fd, ADS1256_CMD_SYNC);
    ads1256_send_command(fd, ADS1256_CMD_WAKEUP);
    
    // Display all register values
    printf("\nADS1256 Register Dump:\n");
    ads1256_dump_registers(fd);
    
    // Continuous sampling demo
    printf("\nSampling for 50ms:\n");
    printf("------------------\n");
    
    const int max_samples = 1000;  // More than enough for 50ms at 15kSPS
    double *samples = (double*)malloc(max_samples * sizeof(double));
    
    if (!samples) {
        printf("Memory allocation failed\n");
        close(fd);
        return EXIT_FAILURE;
    }
    
    int actual_samples;
    
    // Sample for 50ms
    result = ads1256_sample(fd, 50, samples, max_samples, &actual_samples);
    if (result != ADS1256_OK) {
        printf("Sampling failed: %s\n", ads1256_strerror(result));
        free(samples);
        close(fd);
        return EXIT_FAILURE;
    }
    
    printf("Collected %d samples\n", actual_samples);
    
    // Display first 10 samples
    int display_count = (actual_samples < 10) ? actual_samples : 10;
    for (int i = 0; i < display_count; i++) {
        printf("Sample %d: %.6f V\n", i, samples[i]);
    }
    
    // Calculate average
    double sum = 0.0;
    for (int i = 0; i < actual_samples; i++) {
        sum += samples[i];
    }
    
    printf("\nAverage voltage: %.6f V\n", sum / actual_samples);
    
    // Clean up
    free(samples);
    close(fd);
    
    printf("\n=== Example Complete ===\n");
    
    return EXIT_SUCCESS;
}
