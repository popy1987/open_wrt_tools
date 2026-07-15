# cwc — Change Wi-Fi Channel (OpenWrt SDK cross-build)
#
#   export SDK=/path/to/openwrt-sdk-25.12.x-mediatek-filogic_x86_64_gcc-...
#   make STAGING_DIR=$SDK/staging_dir/target-aarch64_cortex-a53
#
# Install on router:
#   scp cwc root@192.168.1.1:/usr/local/bin/
#   ssh root@192.168.1.1 "chmod +x /usr/local/bin/cwc"

TARGET := cwc
SRCS := cwc.c reg.c wifi_uci.c probe.c
OBJS := $(SRCS:.c=.o)

CFLAGS += -Wall -Wextra -Os -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS += -luci -lubox

STAGING_DIR ?=

ifneq ($(STAGING_DIR),)
  TOOLCHAIN_DIR := $(firstword $(wildcard $(STAGING_DIR)/../toolchain-*))
  ifeq ($(TOOLCHAIN_DIR),)
    $(error Cannot find toolchain under $(STAGING_DIR)/../toolchain-*)
  endif
  CC := $(firstword $(wildcard $(TOOLCHAIN_DIR)/bin/*-openwrt-linux*-gcc))
  ifeq ($(CC),)
    CC := $(firstword $(wildcard $(TOOLCHAIN_DIR)/bin/*-gcc))
  endif
  CFLAGS += -I$(STAGING_DIR)/usr/include
  LDFLAGS := -L$(STAGING_DIR)/usr/lib -L$(STAGING_DIR)/root-mediatek/lib $(LDFLAGS)
  $(info Cross CC=$(CC))
else
  $(warning STAGING_DIR not set — building for host (libuci must be installed); use OpenWrt SDK for router binary)
endif

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)
	$(STRIP) $@ 2>/dev/null || true

%.o: %.c reg.h wifi_uci.h probe.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
