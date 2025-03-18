#
# Makefile 
#

PROGRAM = ads1256
VERS = 3.3

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

# Cesty k hlavičkovým souborům a knihovnám
INCLUDE_PATH = -I$(HOME)/include
LIB_PATH = -L$(HOME)/lib

# Other parameters (-Wall -Wextra -pedantic)
CFLAGS = -Wall -Wextra $(OPT) $(INCLUDE_PATH) #-pedantic 

# Knihovny pro linkování
LIB = -lspi #-lwiringPi # -lm -lefence

# Cilum build, install, uninstall, clean a dist neodpovida primo zadny soubor
# (predstirany '.PHONY' target)

.PHONY: build
.PHONY: lib
.PHONY: install
.PHONY: uninstall
.PHONY: clean
.PHONY: dist

# list of valid suffixes through the use of the .SUFFIXES special target.
#.SUFFIXES: .c .o

build: $(PROGRAM)

# Sestavení statické knihovny
lib: $(STATIC_LIB)

$(STATIC_LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

install: build lib
	mkdir -p $(HOME)/bin
	mkdir -p $(HOME)/lib
	mkdir -p $(HOME)/include
	cp $(PROGRAM) $(HOME)/bin
	cp $(STATIC_LIB) $(HOME)/lib
	cp $(HEAD) $(HOME)/include

uninstall:
	rm -f $(HOME)/bin/$(PROGRAM)
	rm -f $(HOME)/lib/$(STATIC_LIB)
	rm -f $(HOME)/include/$(HEAD)

clean:
	rm -f *.o $(PROGRAM) $(STATIC_LIB)

dist:
	tar czf $(PROGRAM)-$(VERS).tgz $(SRC) $(HEAD) Makefile README.md

$(PROGRAM): $(OBJ) Makefile
	$(CC) $(OBJ) $(LIB_PATH) $(LIB) -o $(PROGRAM)

%.o: %.c $(HEAD) Makefile
	$(CC) $(CFLAGS) -c $<
