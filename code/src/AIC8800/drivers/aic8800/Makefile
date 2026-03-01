CONFIG_AIC_LOADFW_SUPPORT := m
CONFIG_AIC8800_WLAN_SUPPORT := m


obj-$(CONFIG_AIC_LOADFW_SUPPORT)    += aic_load_fw/
obj-$(CONFIG_AIC8800_WLAN_SUPPORT) += aic8800_fdrv/


# Platform support list
CONFIG_PLATFORM_ROCKCHIP ?= n
CONFIG_PLATFORM_ALLWINNER ?= n
CONFIG_PLATFORM_AMLOGIC ?= n
CONFIG_PLATFORM_UBUNTU ?= y

ifeq ($(CONFIG_PLATFORM_ROCKCHIP), y)
# Override when building for Rockchip:
#   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=/path/to/toolchain-prefix-
KDIR  ?= /path/to/kernel
ARCH ?= arm
CROSS_COMPILE ?= /path/to/toolchain-prefix-
ccflags-y += -DANDROID_PLATFORM
endif

ifeq ($(CONFIG_PLATFORM_ALLWINNER), y)
# Override when building for Allwinner:
#   make KDIR=/path/to/kernel ARCH=arm64 CROSS_COMPILE=/path/to/toolchain-prefix-
KDIR  ?= /path/to/kernel
ARCH ?= arm64
CROSS_COMPILE ?= /path/to/toolchain-prefix-
ccflags-y += -DANDROID_PLATFORM
endif

ifeq ($(CONFIG_PLATFORM_AMLOGIC), y)
ccflags-y += -DANDROID_PLATFORM
# Override when building for Amlogic:
#   make KDIR=/path/to/kernel ARCH=arm CROSS_COMPILE=/path/to/toolchain-prefix-
ARCH ?= arm
CROSS_COMPILE ?= /path/to/toolchain-prefix-
KDIR ?= /path/to/kernel

endif

ifeq ($(CONFIG_PLATFORM_UBUNTU), y)
KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   ?= $(shell pwd)
KVER ?= $(shell uname -r)
MODDESTDIR ?= /lib/modules/$(KVER)/kernel/drivers/net/wireless/aic8800
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=
ccflags-y += -Wno-implicit-fallthrough -Wno-unused-function -Wno-unused-variable
endif


all: modules
modules:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install:
	mkdir -p $(MODDESTDIR)
	install -p -m 644 aic_load_fw/aic_load_fw.ko  $(MODDESTDIR)/
	install -p -m 644 aic8800_fdrv/aic8800_fdrv.ko  $(MODDESTDIR)/
	/sbin/depmod -a ${KVER}

uninstall:
	rm -rfv $(MODDESTDIR)/aic_load_fw.ko
	rm -rfv $(MODDESTDIR)/aic8800_fdrv.ko
	/sbin/depmod -a ${KVER}

clean:
	cd aic_load_fw/;make clean;cd ..
	cd aic8800_fdrv/;make clean;cd ..
	rm -rf modules.order Module.symvers .tmp_versions/
