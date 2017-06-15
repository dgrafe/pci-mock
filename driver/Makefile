obj-m := mock-demo.o 
KDIR  := /lib/modules/$(shell uname -r)/build

VERSION=3
PATCHLEVEL=16
SUBLEVEL=7

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

