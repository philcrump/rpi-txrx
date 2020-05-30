# ========================================================================================
# Compile flags

CC = gcc
COPT = -O3 -march=native -mtune=native

# Help detection for ARM SBCs, using devicetree
F_CHECKDTMODEL = $(if $(findstring $(1),$(shell cat /sys/firmware/devicetree/base/model 2>/dev/null)),$(2))
# Jetson Nano is detected correctly
# Raspberry Pi 2 / Zero is detected correctly
DTMODEL_RPI2 = Raspberry Pi 2 Model B 
DTMODEL_RPI3 = Raspberry Pi 3 Model B 
DTMODEL_RPI4 = Raspberry Pi 4 Model B 
COPT_RPI2 = -mfpu=neon-vfpv4
COPT_RPI34 = -mfpu=neon-fp-armv8
COPT += $(call F_CHECKDTMODEL,$(DTMODEL_RPI2),$(COPT_RPI2))
COPT += $(call F_CHECKDTMODEL,$(DTMODEL_RPI3),$(COPT_RPI34))
COPT += $(call F_CHECKDTMODEL,$(DTMODEL_RPI4),$(COPT_RPI34))
# Required for NEON, warning: may lead to loss of floating-point precision
COPT += -funsafe-math-optimizations

CFLAGS = -Wall -Wextra -Wpedantic -Werror -std=gnu11 -D_GNU_SOURCE -DNEON_OPTS -pthread
CFLAGS += -D BUILD_VERSION="\"$(shell git describe --dirty --always)\""	\
		-D BUILD_DATE="\"$(shell date '+%Y-%m-%d_%H:%M:%S')\"" \

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
