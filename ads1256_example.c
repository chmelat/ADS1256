/* 
 *  ADS1256,  24 bit, low-noise ADC, 4 Channels, up to 302 kSPS  
 *  V1.0/27.8.2021
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
 */


#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>

#include "spi_base.h"
#include "ads1256_lib.h"


int main(void)
{
  uint32_t mode = 1; /* SPI mode  (device ads1256) */
  uint8_t bpw = 0; /* SPI bits per word, 0 is default value */
  uint32_t speed = 256000; /* SPI speed (Hz) */
  char *device = "/dev/spidev0.0"; 
  int fd;
  int gain = 1;
  int ch = 1;

  verbose_spi = 1;

/* Open SPI device */
  fd = open(device, O_RDWR);
  if (fd < 0)
    pabort("Can't open SPI device");

/* Set SPI mode */
  set_spi_mode(fd,mode);

/* Set SPI bits per word */
  set_spi_bpw(fd,bpw);

/* Set SPI ispeed (HZ) */
  set_spi_speed(fd,speed);


//  printf("SPI mode: %d\n", mode);
//  printf("SPI bits per word: %d\n", bpw);
//  printf("SPI speed: %d Hz (%d KHz)\n", speed, speed/1000);

//  tr.bits_per_word = bpw;
#if 0
  printf("tr.bits_per_word: %d\n", tr.bits_per_word);
  printf("tr.speed_hz: %d\n", tr.speed_hz);
  printf("tr.delay_usecs: %d\n", tr.delay_usecs);
#endif  
//  tr.delay_usecs = delay;

  tr.tx_buf = (unsigned long)tx;
  tr.rx_buf = (unsigned long)rx;

//  print_reg();

   
//  wakeup_1256(fd);
  send_command_1256(fd,RESET);
  set_channel_1256(fd,ch);
  set_gain_1256(fd,gain);
  set_operating_mode_1256(fd,0);
  set_drate_1256(fd,1);
  set_buffer_1256(fd,0);
  send_command_1256(fd,SELFCAL);
  send_command_1256(fd,SYNC);
  send_command_1256(fd,WAKEUP);

//  printf("Channel = %d\n", set_channel(fd, 0));
//  printf("Gain = %d\n", set_gain_1256(fd, 0));

//  printf("Start mereni...\n");
  sample_1256(fd,50);

/* Data recieving ...*/
#if 0  
  {
    for (;;) {
      printf("%f\n", voltage_1256(fd));
  //    sleep(1);
    }
  }
#endif   

  return 0;
}	
