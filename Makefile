CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?=

# Target only AArch64 (arm64)
# ARCH := $(shell uname -m)
# ifneq ($(ARCH),aarch64)
# $(error This Makefile targets arm64/aarch64 only. Current ARCH is $(ARCH))
# endif

TARGET := ipmb_host_userspace
SRC := ipmb_host_userspace.c ipmi_devintf_userspace.c

PKG_CONFIG ?= pkg-config
FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(LDFLAGS) $(FUSE_LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
