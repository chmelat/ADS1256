/* ADS1256 library */

/* ADS1256 Register */
#define STATUS 0x00
#define MUX 0x01
#define ADCON 0x02
#define DRATE 0x03
#define IO 0x04
#define OFC0 0x05
#define OFC1 0x06
#define OFC2 0x07
#define FSC0 0x08
#define FSC1 0x09
#define FSC2 0x0A

/* ADS1256 Command */
#define WAKEUP 0x00
#define RDATA 0x01
#define RDATAC 0x03
#define SDATAC 0x0f
#define RREG 0x10
#define WREG 0x50
#define SELFCAL 0xF0
#define SELFOCAL 0xF1
#define SELFGCAL 0xF2
#define SYSOCAL 0xF3
#define SYSGCAL 0xF4
#define SYNC 0xFC
#define STANDBY 0xFD
#define RESET 0xFE


/* Declare global variables */


/* Declare global functions */
extern void set_operating_mode_1256(int fd, uint8_t mode);
extern void set_conversion_mode_1256(int fd, uint8_t mode);
extern void set_channel_1256(int fd, int ch);
 extern int set_gain_1256(int fd, uint8_t gain);
extern void set_buffer_1256(int fd, uint8_t mode);
extern void set_drate_1256(int fd, uint8_t mode);
extern double voltage_1256(int fd);
extern void sample_1256(int fd, int t);
extern void send_command_1256(int fd, uint8_t command);
#if 0
extern void init_1256(void);
#endif
