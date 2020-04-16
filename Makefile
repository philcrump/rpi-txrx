# ========================================================================================
# Compile flags

CC = gcc
COPT = -O3
CFLAGS = -Wall -Wextra -Wpedantic -Werror -std=gnu11 -D_GNU_SOURCE
CFLAGS += -D BUILD_VERSION="\"$(shell git describe --dirty --always)\""	\
		-D BUILD_DATE="\"$(shell date '+%Y-%m-%d_%H:%M:%S')\"" \
		-pthread -march=native -mtune=native -mfpu=neon -mvectorize-with-neon-quad -DNEON_OPTS

BIN = txrx

# ========================================================================================
# Source files

SRCDIR = .

SRC = $(SRCDIR)/screen.c \
		$(SRCDIR)/graphics.c \
		$(SRCDIR)/lime.c \
		$(SRCDIR)/fft.c \
		$(SRCDIR)/mouse.c \
		$(SRCDIR)/timing.c \
		$(SRCDIR)/temperature.c \
		$(SRCDIR)/font/font.c \
		$(SRCDIR)/font/dejavu_sans_32.c \
		$(SRCDIR)/font/dejavu_sans_36.c \
		$(SRCDIR)/font/dejavu_sans_72.c \
		$(SRCDIR)/buffer/buffer_circular.c \
		$(SRCDIR)/if_subsample.c \
		$(SRCDIR)/if_fft.c \
		$(SRCDIR)/if_demod.c \
		$(SRCDIR)/audio.c \
		$(SRCDIR)/touch.c \
		$(SRCDIR)/main.c

# ========================================================================================
# External Libraries

LIBSDIR = 
LIBS = -lm -lLimeSuite -lfftw3f -lasound

# ========================================================================================
# Makerules

all:
	$(CC) $(COPT) $(CFLAGS) $(SRC) -o $(BIN) $(LIBSDIR) $(LIBS)

debug: COPT = -Og -gdwarf -fno-omit-frame-pointer -D__DEBUG
debug: all

clean:
	rm -fv *.o $(BIN)
