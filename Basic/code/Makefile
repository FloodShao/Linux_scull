scull-objs := main.o 
obj-m := scull.o
CURRENT_PATH := ${shell pwd}
CURRENT_KERNEL_PATH := ${shell uname -r}
LINUX_KERNEL_PATH := /usr/src/linux-headers-$(CURRENT_KERNEL_PATH)

all:
	make -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) modules
clean:
	rm *.o    *.order       *.symvers      *.mod.c       *.ko

	
