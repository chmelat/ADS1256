/* 
 *  ADS1256,  24 bit, low-noise ADC, 4 Channels, up to 302 kSPS, f_clk = 7.68 MHz  
 *  V1.0/27.8.2021
 *  V1.1/16.9.2021/Add wakeup
 *  V2.0/22.9.2021/Add wiringPi lib for wait_drdy
 *  V2.1/9.10.2023/Add change in func, set_data_rate() and set_gain(), wringPi dissabled
 *  V2.2/27.2.2024/Bugfix in wait_drdy_c function
 *  V2.3/26.3.2024/Bugfix in DataRate selection
 *  V2.4/27.3.2024/Add calibration delay function
 *
 *  Wiring
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

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
//#include <fcntl.h>
//#include <sys/ioctl.h>
//#include <linux/types.h>
#include <linux/spi/spidev.h>
#if 0
#include <wiringPi.h>
#endif

#include "spi_base.h"
#include "ads1256_lib.h"


/* Define global variables */
#if 0
#define DRDY_PIN 7 /* GPIO pin (pin 7 on board) to read DRDY */
#endif


/* Local variables of module */
const static double Vref = 2.037;  /* Voltage reference [V] */
static uint8_t reg_conf[11]; /* Configuration registers */
static uint8_t reg_data[3];  /* Data register */
static int gain = 1;  /* Amplifier gain, default gain is 1 */
static int channel = 1; /* Selected Channel, default channel is 1 */
static int drate = 0; /* Data Rate mode, */

/* Power of two (2^n) */
static int power(int n)
{
  int p = 1;	
  while (n-- > 0) 
    p *= 2;
  return p;        
}

/* Read one byte configuration register at address 'addr' */
static void read_reg(int fd, uint8_t addr)
{
  tx[0]=0b00010000 | (addr & 0b00001111); // Read Configuration register 
  tx[1]=0b00000000;
  tx[2]=0b00000000;
  spi_trans(fd, 3);
  reg_conf[addr] = rx[2];
//  spi_write(fd,tx,2);
//  spi_read(fd,rx,1); 
//  reg_conf[addr] = rx[0];
//
//  print_binary(tx[0]); putchar('\n');
}

/* Write one byte configuration register at address 'addr' */
static void write_reg(int fd, uint8_t addr)
{
  tx[0] = 0b01010000 | (addr & 0b00001111); // Write 1 byte of reg on addr
  tx[1] = 0b00000000;
  tx[2] = reg_conf[addr];
  spi_trans(fd, 3); 
}

/* Print all registers */
static void print_reg(int fd)
{
  int i;

  printf("\nADS1256 Register:\n");
    for (i=0; i<11; i++) {
      read_reg(fd, i);	    
      print_binary(reg_conf[i]); putchar('\n');
    }
    putchar('\n');
}

/* Wait for data ready - read from register (slow)  */
static void wait_drdy_c(int fd)
{
  uint8_t r = 1;

  while (r) {
    read_reg(fd,STATUS);
    r = (reg_conf[STATUS] & 0b00000001);
    if (!r)
      break;
    usleep(1000);  /* 1000 us */
  }
}

#if 0
/* Wait for data ready read from GPIO (fast) */
static void wait_drdy(int fd)
{
  while (digitalRead(DRDY_PIN))
   ; 
}

/* Initially wiringPiSetup */
void init_1256(void)
{
  wiringPiSetup();
  if (errno != 0) {
    perror("wiringPiSetup");
    exit(EXIT_FAILURE);
  }
  pinMode (DRDY_PIN, INPUT); /* DRDY input  */
}
#endif

/*
 * Set operating mode
 * 0-Normal mode (default), 1-Duty-cycle mode, 2-Turbo mode, 
 */
void set_operating_mode_1256(int fd, uint8_t mode)
{
  read_reg(fd,MUX);

  if (mode == 0) {
    reg_conf[MUX] &= 0b11100111;
    if (verbose_spi) 
      printf("Normal mode (0) ... ");  
  } else if (mode == 1) {
    reg_conf[MUX] &= 0b11100111;
    reg_conf[MUX] |= 0b00001000;
    if (verbose_spi) 
      printf("Duty-cycle mode 1) ... ");
  } else if (mode == 2) {
    reg_conf[MUX] &= 0b11100111;
    reg_conf[MUX] |= 0b00010000;
    if (verbose_spi) 
      printf("Turbo mode (2) ... ");  
  } else {
    printf("set_operating_mode_1256: Unknown mode!\n");
    exit(EXIT_FAILURE);  
  }

    write_reg(fd,1);
  
  if (verbose_spi) 
    printf("ok\n"); 
}


/*
 * Set Conversion mode
 * 0-Single shot mode (default), 1-Continuous conversion mode 
 */
void set_conversion_mode_1256(int fd, uint8_t mode)
{
  read_reg(fd,MUX);

  if (mode == 0) {
    reg_conf[MUX] &= 0b11111101;
    if (verbose_spi) 
      printf("Single-shot mode (0) ... ");  
  }
  else if (mode == 1) {
    reg_conf[MUX] |= 0b00000010;
    if (verbose_spi) 
      printf("Continuous conversion mode (1) ... ");
  }
  else {
    printf("set_conversion_mode_1256: Unknown mode!\n");
    exit(EXIT_FAILURE);  
  }

    write_reg(fd,MUX);

  if (verbose_spi)
    printf("ok\n"); 
}


/*
 *  If ch > 0 then set channel and return 0
 */
void set_channel_1256(int fd, int ch)
{
  read_reg(fd,MUX);

//  print_reg();

  if (ch == 1) {   /* +AIN0, -AIN1 */
    reg_conf[MUX] = 0b00000001;
    channel = 1; 
  } else if (ch == 2) {   /* +AIN2, -AIN3 */
    reg_conf[MUX] = 0b00100011;  
    channel = 2; 
  } else if (ch == 3) {   /* +AIN4, -AIN5 */
    reg_conf[MUX] = 0b01000101;  
    channel = 3; 
  } else if (ch == 4) {   /* +AIN6, -AIN7 */
    reg_conf[MUX] = 0b01100111;  
    channel = 4; 
  } else {
    printf("set_channel_1256: Channel must be from 1..4, not %d!\n",ch);
    exit(EXIT_FAILURE);    
  }
  
  if (verbose_spi) 
    printf("Set channel to %d ... ",ch); 

    write_reg(fd,MUX);

  if (verbose_spi)  
    printf("ok\n");
  	  
//  print_reg();
}


/*
 *  If gain > 0 then set gain and return 0, if gain == 0 then return real gain
 */
int set_gain_1256(int fd, uint8_t ng)
{
/* Gain,0-1x, 1-2x, 2-4x, ... 6-64x */ 
const static uint8_t vec_gain[7] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06};
  uint8_t g = 0;

  read_reg(fd,ADCON);

//  print_reg();
  if (ng == 0) { // return actual gain
    g = (reg_conf[ADCON] & 0b00000111);
    g = power(g);
    gain = g;    
  } else { /* Reset gain */
    reg_conf[ADCON] &= 0b11111000;
  }

/* Set gain */
  if (ng == 1)
    reg_conf[ADCON] |= vec_gain[0];
  else if (ng == 2)
    reg_conf[ADCON] |= vec_gain[1];
  else if (ng == 4)
    reg_conf[ADCON] |= vec_gain[2];
  else if (ng == 8)
    reg_conf[ADCON] |= vec_gain[3];
  else if (ng == 16)
    reg_conf[ADCON] |= vec_gain[4];
  else if (ng == 32)
    reg_conf[ADCON] |= vec_gain[5];
  else if (ng == 64)
    reg_conf[ADCON] |= vec_gain[6];
  else {
    printf("set_gain_1256: Gain must be from 0,1,2,4,8,..,64, not %d!\n",ng);
    exit(EXIT_FAILURE);    
  }

  if (g == 0) {
    if (verbose_spi)	  
      printf("Set gain to %d ... ",ng);  

    write_reg(fd,ADCON);
    gain = ng;

    if (verbose_spi)
      printf("ok\n");
//  print_reg();
  }

  return (int)g; 
}

/*
 *  Set Buffer, Enable buffer dramatically increase impedance to cca 80 MOhm
 *  0-Buffer disabled (default), 1-Buffer enabled, 
 */
void set_buffer_1256(int fd, uint8_t mode)
{
  read_reg(fd,STATUS);

  if (mode == 0) {
    reg_conf[STATUS] &= 0b11111101;
    if (verbose_spi)
      printf("Buffer disable ... ");  
  } else if (mode == 1) {
    reg_conf[STATUS] |= 0b00000010;
    if (verbose_spi)
      printf("Buffer enable ... ");
  } else {
    printf("set_buffer_1256: Unknown buffer mode!\n");
    exit(EXIT_FAILURE);  
  }

  write_reg(fd,STATUS);

  if (verbose_spi)
    printf("ok\n"); 
}

/*
 *  Set Data Rate, {mode-SPS}:
 *  0-30000(default),1-15000,2-7500,3-3750,4-2000,5-1000,6-500,7-100,8-60,
 *  9-50,10-30,11-25,12-15,13-10,14-5,15-2.5 
 */
void set_drate_1256(int fd, uint8_t mode)
{

/* Data rate */
const static uint8_t vec_data_rate[16] =
  {0xF0,0xE0,0xD0,0xC0,0xB0,0xA1,0x92,0x82,0x72,0x63,0x53,0x43,0x33,0x23,0x13,0x03};
const static char *mode_data_rate[16] =
  {"30000","15000","7500","3750","2000","1000","500","100","60","50","30","25","15","10","5","2.5"};


// read_reg(fd,DRATE);

  if (mode >= 0 && mode <= 15) {
    reg_conf[DRATE] = vec_data_rate[mode];
  } else {
    printf("set_drate_1256: Data rate mode must be from int. <0,..,15>, %d is wrong!\n",mode);
    exit(EXIT_FAILURE);  
  }

  if (verbose_spi) {
    printf("Set Data Rate to %s SPS ... ",mode_data_rate[mode]);
  }  

  write_reg(fd,DRATE);
  drate = mode;

  if (verbose_spi)
    printf("ok\n");
//  print_reg(fd);
}

/*
 *  Read data register
 */
static void read_data(int fd)
{
  wait_drdy_c(fd);

  tx[0] = RDATA;   
  tx[1] = tx[2] = tx[3]=0b00000000;

  spi_trans(fd, 4); 

  reg_data[0] = rx[1];  
  reg_data[1] = rx[2];  
  reg_data[2] = rx[3];  

#if 0  
  tx[0]=0b00000001;   /* Read data */
  spi_write(fd,tx,1);
  wait_drdy_c(fd);
  spi_read(fd,rx,3);
  reg_data[0] = rx[0];
  reg_data[1] = rx[1];
  reg_data[2] = rx[2];
#endif  
}


static uint32_t get24bit(uint8_t *e)
{
  uint32_t n;

//  n = (uint32_t)*(e+2) + 256*(uint32_t)*(e+1) + 65536*(uint32_t)*(e);
  n = (uint32_t)*(e+2) + (uint32_t)(*(e+1) << 8) + (uint32_t)(*e << 16);
  return n; 
}

/* Convert integer number from ADC to voltage */
static double n2V(long int n)
{
  double V;
  double FS = 2*Vref/gain;

  if (n >= 0x800000) { /* Negative voltage */
    n -= 0xFFFFFF + 1;
  }

  V = FS*(double)n/0x800000;

  return V;
}


/* Voltage [V]  */
double voltage_1256(int fd)
{
  uint32_t n;
  double V;

  read_data(fd);
  n = get24bit(&reg_data[0]);
  V = n2V((long int)n);

  return V;
}

/* Sample (V), time t (ms)  */
void sample_1256(int fd, int t)
{
  int n = 0; /* Number of samples */
  long int m;
  int i;
  double *sample; /* (V) */
  uint8_t *rxc; /* Buffer for Continual recieving data */
/* vector of samples per time (1 ms) */
  const float vec_spt[16] = {30,15,7.5,3.75,2,1,0.5,0.1,0.06,0.05,0.03,0.025,0.015,0.01,0.005,0.0025};

  if (drate >=0 && drate <=15) {
      n = t*vec_spt[drate];
  } else {
    printf("sample_1256: drate must be from int. <0,..,15>, %d is wrong!\n",drate);
    exit(EXIT_FAILURE);  
  }

  if (n>0) { /* Data allocation */

    rxc = (uint8_t*)calloc(3*n,sizeof(uint8_t));
    if (rxc == NULL) {
      printf("sample_1256: No memory for rxc allocation!\n");
      exit(EXIT_FAILURE);  
    }

    sample = (double*)malloc(n*sizeof(double));
    if (sample == NULL) {
      printf("sample_1256: No memory for sample allocation!\n");
      exit(EXIT_FAILURE);  
    }
  }
  else {
    printf("sample_1256: Incorrect length sample!\n");
    exit(EXIT_FAILURE);  
  }


  tr.rx_buf=(unsigned long)rxc;
  tx[0] = RDATAC;   /* Read data Continuously */
  spi_write(fd,tx,1);
  
  for (i=0; i<n; i++) { 
    wait_drdy_c(fd);
    spi_read(fd,&rxc[3*i],3);
  }
  
  tx[0] = SDATAC;   /* Stop Read data Continuously */
  spi_write(fd,tx,1); 

  for (i=0; i<n; i++) {
    m = (long int)get24bit(&rxc[3*i]);
    sample[i] = n2V(m);
  }


  /* Print read raw data ... */
  for (i=0; i<n; i++) {
    printf("%f\n",sample[i]);
  }

  tr.rx_buf=(unsigned long)rx; /* Back to small rx buffer */

  free(rxc);
  free(sample);
}


/*
 *  Print verbose (ads1256)
 */
static void verbose_1256(uint8_t command)
{
  switch (command)
  {
    case WAKEUP: 
      printf("Wake-up ... "); break;
    case RDATA:
      printf("Read data ... "); break;
    case RDATAC:
      printf("Read data continuously ... "); break;
    case SDATAC:
      printf("Stop read data continuously ... "); break;
    case RREG:
      printf("Read register ... "); break;
    case WREG:
      printf("Write register ... "); break;
    case SELFCAL:
      printf("Self calibration ... "); break;
    case SELFOCAL:
      printf("Self offset calibration ... "); break;
    case SELFGCAL:
      printf("Self gain calibration ... "); break;
    case SYSOCAL:
      printf("System offset calibration ... "); break;
    case SYSGCAL:
      printf("System gain calibration ... "); break;
    case SYNC:
      printf("Sync ... "); break;
    case STANDBY:
      printf("Standby ... "); break;
    case RESET:
      printf("Reset ... "); break;
    default:  
      printf("verbose_1256: Unknown command!\n");
      exit(EXIT_FAILURE);
  }
}

/*
 *  Send command to ads1256
 */
void send_command_1256(int fd, uint8_t command)
{
  if (verbose_spi)
    verbose_1256(command);

  tx[0] = command;
  spi_write(fd,tx,1);

  if (command == SYNC)
    usleep(4);
  else
    wait_drdy_c(fd);

  if (verbose_spi)
    printf("ok\n");
}

/*
 *  Delay after self calibration functions
 */
void calibration_delay_1256(uint8_t command)
{
/* Self-Calibration Timing [us] */  
  const static int self_calibration[16]={892,896,1029,1300,2000,3600,6600,31200,50900,61800,101300,123200,202100,307200,613800,1227200};
/*Self Offset and System Offset Calibration Timing [us] for DR {30000..2.5} */
  const static int self_and_system_offset[16]={387,453,587,853,1300,2300,4300,20300,33700,40300,67000,80300,133700,200300,400300,800300};

  if ( command == SELFCAL)
    usleep(self_calibration[drate]);  
  else if ( command == SELFOCAL || command == SYSOCAL )
    usleep(self_and_system_offset[drate]);  
}
