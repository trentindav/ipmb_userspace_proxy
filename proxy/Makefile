CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?=

ARCH := $(shell uname -m)
# ifneq ($(ARCH),aarch64)
# $(error This Makefile targets arm64/aarch64 only. Current ARCH is $(ARCH))
# endif

TARGET := ipmi_lanplus_proxy
SRC := ipmi_lanplus_proxy.c

# PKG_CONFIG ?= pkg-config
# FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null)
# FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
