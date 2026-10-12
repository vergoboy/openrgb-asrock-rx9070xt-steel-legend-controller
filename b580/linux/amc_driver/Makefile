# Makefile for the out-of-tree AMC I2C client driver.
# Build against the running kernel headers:
#   make
# Load (temporarily / reversibly):
#   sudo insmod amc_i2c.ko
# Bind to the amc client if not auto-bound:
#   echo 28-0036 | sudo tee /sys/bus/i2c/drivers/amc_i2c/bind
# Sysfs node (once bound):
#   /sys/bus/i2c/devices/28-0036/{addr,reg,value,wr,rd,dump}
# Unload / unbind (reversible):
#   sudo sh -c 'echo 28-0036 > /sys/bus/i2c/drivers/amc_i2c/unbind'
#   sudo rmmod amc_i2c

KDIR := /usr/lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m += amc_i2c.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo install -Dm644 amc_i2c.ko /lib/modules/$(shell uname -r)/extra/amc_i2c.ko
	sudo depmod -a

uninstall:
	sudo rm -f /lib/modules/$(shell uname -r)/extra/amc_i2c.ko
	sudo depmod -a