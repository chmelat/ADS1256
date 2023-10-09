#
# Makefile 
#

PROGRAM=ads1256
VERS = 1.0

# Seznam souboru
SRC=ads1256_example.c ads1256_lib.c spi_base.c 
OBJ=$(SRC:.c=.o)
HEAD=ads1256_lib.h spi_base.h

# C translator (clang, gcc, ..)
CC = clang

# Optimalization (-O0 -g = debug, -O0 -pg = gprof, -O2 = normal)
OPT = -O2

# Other parameters (-Wall -Wextra -pedantic)
CFLAGS = -Wall -Wextra #-pedantic 

LIB = #-lwiringPi # -lm -lefence

# Cilum build, install, uninstall, clean a dist neodpovida primo zadny soubor
# (predstirany '.PHONY' target)

.PHONY: build
.PHONY: install
.PHONY: uninstall
.PHONY: clean
.PHONY: dist

# list of valid suffixes through the use of the .SUFFIXES special target.
#.SUFFIXES: .c .o

build: $(PROGRAM)

install: build
	cp $(PROGRAM) ~/bin

uninstall:
	rm -f ~/bin/$(PROGRAM)

clean:
	rm -f *.o $(PROGRAM)

dist:
	tar czf $(PROGRAM)-$(VERS).tgz $(SRC) $(HEAD) Makefile

$(PROGRAM): $(OBJ) Makefile
	$(CC) $(OBJ) $(LIB) -o $(PROGRAM)

%.o: %.c $(HEAD) Makefile
	$(CC) $(CFLAGS) $(OPT) -c $<

# Zavislost objektovych souboru (v BSD make funkcni)
#.c.o: $(HEAD) Makefile
#	$(CC) $(CFLAGS) $(OPT) -c $<
