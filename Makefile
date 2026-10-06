#
# Makefile 
#

PROGRAM = ads1256
CAL = ads1256_cal
VERS = 4.4

# Zdrojové soubory pro program
SRC = ads1256_example.c ads1256_lib.c
OBJ = $(SRC:.c=.o)
HEAD = ads1256_lib.h

# Zdrojové soubory pro knihovnu
LIB_NAME = ads1256
LIB_SRC = ads1256_lib.c
LIB_OBJ = $(LIB_SRC:.c=.o)
STATIC_LIB = lib$(LIB_NAME).a

# C translator (clang, gcc, ..)
CC = clang
AR = ar
RANLIB = ranlib

# Optimalization (-O0 -g = debug, -O0 -pg = gprof, -O2 = normal)
OPT = -O2

# Other parameters (-Wall -Wextra -pedantic)
CFLAGS = -Wall -Wextra $(OPT) #-pedantic 

# Cilum build, install, uninstall, clean a dist neodpovida primo zadny soubor
# (predstirany '.PHONY' target)

.PHONY: build lib install uninstall clean dist test hwtest

build: $(PROGRAM) $(CAL)

# Sestavení statické knihovny
lib: $(STATIC_LIB)

$(STATIC_LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: build lib
	mkdir -p $(HOME)/lib
	mkdir -p $(HOME)/include
	cp $(STATIC_LIB) $(HOME)/lib
	cp $(HEAD) $(HOME)/include

uninstall:
	rm -f $(HOME)/lib/$(STATIC_LIB)
	rm -f $(HOME)/include/$(HEAD)

clean:
	rm -f *.o $(PROGRAM) $(CAL) $(STATIC_LIB) test_ads1256 hwtest_ads1256

dist:
	tar czf $(PROGRAM)-$(VERS).tgz $(SRC) $(HEAD) ads1256_cal.c test_ads1256.c hwtest_ads1256.c Makefile README.md LICENSE

# Hardware-free test (emulated ADS1256)
test: test_ads1256.c $(LIB_SRC) $(HEAD)
	$(CC) $(CFLAGS) test_ads1256.c $(LIB_SRC) -lm -o test_ads1256
	./test_ads1256

# Hardware self-check (connected ADS1256 with DRDY; args: make hwtest HWARGS="/dev/gpiochip1 3")
hwtest: hwtest_ads1256.c $(LIB_SRC) $(HEAD)
	$(CC) $(CFLAGS) hwtest_ads1256.c -lm -o hwtest_ads1256
	./hwtest_ads1256 $(HWARGS)

$(PROGRAM): $(OBJ) Makefile
	$(CC) $(OBJ) -o $(PROGRAM)

# System calibration tool
$(CAL): ads1256_cal.o $(LIB_OBJ) Makefile
	$(CC) ads1256_cal.o $(LIB_OBJ) -o $(CAL)

%.o: %.c $(HEAD) Makefile
	$(CC) $(CFLAGS) -c $<
