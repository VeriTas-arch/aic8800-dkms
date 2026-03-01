CONFIG_USB_SUPPORT =y
CONFIG_RFTEST =y
CONFIG_USB_BT =y
CONFIG_USB_MSG_EP = y
CONFIG_USB_NO_TRANS_DMA_MAP = n
CONFIG_M2D_OTA_AUTO_SUPPORT = n
CONFIG_LINK_DET_5G = y
# Need to set fw path in BOARD_KERNEL_CMDLINE
CONFIG_USE_FW_REQUEST = n
CONFIG_PREALLOC_RX_SKB = n
CONFIG_PREALLOC_TXQ = y

# Platform support list
CONFIG_PLATFORM_ROCKCHIP ?= n
CONFIG_PLATFORM_ALLWINNER ?= n
CONFIG_PLATFORM_AMLOGIC ?= n
CONFIG_PLATFORM_UBUNTU ?= y

CONFIG_AIC_LOADFW_SUPPORT = m
MODULE_NAME = aic_load_fw
# CONFIG_AIC_FW_PATH = "/vendor/etc/firmware"
# export CONFIG_AIC_FW_PATH

ifeq ($(CONFIG_USB_SUPPORT), y)
ccflags-y += -DAICWF_USB_SUPPORT
endif
ccflags-$(CONFIG_RFTEST) += -DCONFIG_RFTEST
ccflags-$(CONFIG_USB_BT) += -DCONFIG_USB_BT
ccflags-$(CONFIG_USB_MSG_EP) += -DCONFIG_USB_MSG_EP
ccflags-$(CONFIG_USB_NO_TRANS_DMA_MAP) += -DCONFIG_USB_NO_TRANS_DMA_MAP
ccflags-$(CONFIG_M2D_OTA_AUTO_SUPPORT) += -DCONFIG_M2D_OTA_AUTO_SUPPORT
ccflags-$(CONFIG_LINK_DET_5G) += -DCONFIG_LINK_DET_5G
ccflags-$(CONFIG_USE_FW_REQUEST) += -DCONFIG_USE_FW_REQUEST
ccflags-$(CONFIG_PREALLOC_RX_SKB) += -DCONFIG_PREALLOC_RX_SKB
ccflags-$(CONFIG_PREALLOC_TXQ) += -DCONFIG_PREALLOC_TXQ


obj-$(CONFIG_AIC_LOADFW_SUPPORT) := $(MODULE_NAME).o
$(MODULE_NAME)-y := 	aic_bluetooth_main.o \
					aicbluetooth.o \
					aicwf_usb.o \
					aic_txrxif.o \
					aicbluetooth_cmds.o \
					md5.o

$(MODULE_NAME)-$(CONFIG_PREALLOC_RX_SKB)	 += aicwf_rx_prealloc.o
$(MODULE_NAME)-$(CONFIG_PREALLOC_TXQ)     += aicwf_txq_prealloc.o



ifeq ($(CONFIG_PLATFORM_ROCKCHIP), y)
ccflags-$(CONFIG_PLATFORM_ROCKCHIP) += -DCONFIG_PLATFORM_ROCKCHIP
# Override when building for Rockchip:
#   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=/path/to/toolchain-prefix-
KDIR  ?= /path/to/kernel
ARCH ?= arm
CROSS_COMPILE ?= /path/to/toolchain-prefix-
endif

ifeq ($(CONFIG_PLATFORM_ALLWINNER), y)
ccflags-$(CONFIG_PLATFORM_ALLWINNER) += -DCONFIG_PLATFORM_ALLWINNER
# Override when building for Allwinner:
#   make KDIR=/path/to/kernel ARCH=arm64 CROSS_COMPILE=/path/to/toolchain-prefix-
KDIR  ?= /path/to/kernel
ARCH ?= arm64
CROSS_COMPILE ?= /path/to/toolchain-prefix-
endif

ifeq ($(CONFIG_PLATFORM_AMLOGIC), y)
ccflags-$(CONFIG_PLATFORM_AMLOGIC) += -DCONFIG_PLATFORM_AMLOGIC
# Override when building for Amlogic:
#   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=/path/to/toolchain-prefix-
ARCH ?= arm
CROSS_COMPILE ?= /path/to/toolchain-prefix-
KDIR ?= /path/to/kernel

endif

ifeq ($(CONFIG_PLATFORM_UBUNTU), y)
ccflags-$(CONFIG_PLATFORM_UBUNTU) += -DCONFIG_PLATFORM_UBUNTU
KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   ?= $(shell pwd)
KVER ?= $(shell uname -r)
MODDESTDIR ?= /lib/modules/$(KVER)/kernel/drivers/net/wireless/aic8800
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=
endif


all: modules
modules:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install:
	mkdir -p $(MODDESTDIR)
	install -p -m 644 $(MODULE_NAME).ko  $(MODDESTDIR)/
	/sbin/depmod -a ${KVER}

uninstall:
	rm -rfv $(MODDESTDIR)/$(MODULE_NAME).ko
	/sbin/depmod -a ${KVER}

clean:
	rm -rf *.o *.ko *.o.* *.mod.* modules.* Module.* .a* .o* .*.o.* *.mod .tmp* .cache.mk
