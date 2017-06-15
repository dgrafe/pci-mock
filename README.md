# Mocking PCI devices in a User Mode Linux environment

We are currently facing the problem that we would like to improve our software tests. But we are highly dependent on the hardware.

Our complex system consists of two FPGAs doing sophisticated calculations on incoming HF-Signals. On top of that our
software is doing the business logik. The connecting interface is the PCI bus and we have a custom
driver that is collecting all the data from the FPGAs and that is managing the queue for the outgoing data.

We would like to establish a hardware abstraction that would allow us to do isolated tests of our software stack.
The reasons are quite obvious, the hardware dependency is expensive, the test process is slow and not automated, the
availability of the hardware is an issue. It's just too much for testing only the business logik in the software stack.


## Simulating the PCI-Bus communication

The data exchanged over the PCI bus is actually pretty simple. It's just a data stream of reports and commands according to
defined data model. There is an ICD fro that. So we basically want to simulate this communication by replacing
the PCI devices with mocks in our test environment. The software stack will hopefully not even notice the simulation
if we can emulate the PCI bus communication at the kernel interfaces to the bus.

### Mocking the FPGA register map

The registers are memory mapped by the driver during the driver initialisation. Accessing data from this memory map
will be automagically synchronized over the bus and with the FPGAs. So if we would initialize the driver with a memory map
pointing to a memory mapped file in the Operating System we would have our hardware abstraction. At least for the data flow part.
But that would already cover 90% of the story. We would then have to write a simulator that is connected to the "other side" of
the memory map and it would have to behave exactly as the real hardware.

### Mocking the interrupt line

We would still have to emulate the interrupt lines that are triggering handlers on special events. However, these handlers
are just functions in the driver (=software= that are run whenever the hardware triggers them via an interrupt. So if we came up
with a mechanism for invoking these functions from the user space we could trigger them from our FPGA user space mocks.


## Putting it all together in a User Mode Linux environment

The User Mode Linux (UML) kernel is running as an ordinary user space process on a separate rootfs file. That's exactly what we
are looking for as we would like to do the test of the software stack on every developer's machine or with several instances
on the CI-Infrastructure.

Crashes of the kernel driver are no longer an issue since it would only kill the user space process and we would just have to start over.
We could import some really weird test data, lay back and just see what happens.

The UML environment would also speed up the deployment as building a new rootfs image and starting a user space process on the dev
machine is obv. much faster then deploying and powering up the whole system in the real world.

### How UML helps us with the interfaces

#### Memory Mapped IO
There is already a solution for the memory mapped IO problem. When booting the kernel process we can give the path to an
[additional file](http://user-mode-linux.sourceforge.net/old/iomem.html) that will be memory mapped by the kernel.
That's our connection from the user space (where the FPGA simulators live) to the kernel driver running in the UML process.

However, the driver would have to request that memory region with a UML specific *find_iomem()* function that would
replace the *pci_iomap* call. So there is not a transparent solution.

#### Interrupt line emulation

UML also solved the interrupt problem. Running as a user space process the UML kernel just cannot register interrupt handlers. UML
came up with an emulation using AIO and file descriptors for each interrupt line. You would install an interrupt handler on a previously
created fd. UML then registers a SIGIO handler and when receiving that signal it's going through the list of file descriptors checking their
pending events. When UML has identified the fd that caused SIGIO the corresponding handler is run.

The file descriptor is a unix domain socket and it is created by our kernel driver. But UML will create the socket file in the Host OS.
That's our trigger for the interrupt line. We can now trigger the interrupt by sending a dummy byte on that socket.

That data is currently just sent for the trigger effect, but maybe we could upgrade this mechanism to a Bus Master DMA emulation where
sending the data blob would realize both the memory transport and irq trigger event.


## Prove of concept: Link status change on a simulated RTL8111 ethernet controller

### How does the hardware behave?

* An interrupt is fired on every link status change when activating the interrupt by setting bit 5 at offset 0x3C
* Bit 5 in the Link Status Register at offset 0x3E indicates that the interrupt was caused by a link status change
* The status (up/down) is indicated by bit 1 in the PHY Status Register at offset 0x6c

### A simple driver running on the real hardware

Building the driver against the currently running kernel on my dev machine:
``` C
make -C /lib/modules/3.2.0-4-amd64/build M=/home/dgrafe/git/pci-mock/driver modules
make[1]: Entering directory '/usr/src/linux-headers-3.2.0-4-amd64'
Makefile:10: *** mixed implicit and normal rules: deprecated syntax
  CC [M]  /home/dgrafe/git/pci-mock/driver/mock-demo.o
  Building modules, stage 2.
  MODPOST 1 modules
  CC      /home/dgrafe/git/pci-mock/driver/mock-demo.mod.o
  LD [M]  /home/dgrafe/git/pci-mock/driver/mock-demo.ko
make[1]: Leaving directory '/usr/src/linux-headers-3.2.0-4-amd64'
```

Installing the driver and plugging in / removing the cable several times gives me this result:
```
insmod ./mock-demo.ko
dmesg
...
[30140.991492] Link status changed to DOWN
[30154.424022] Link status changed to UP
[30161.308828] Link status changed to DOWN
[30164.772680] Link status changed to UP
```

Works as intended. Let's reproduce that exact behaviour in the UML based simulator.


### Building the UML binary

I would recommend building your own UML binary because it is fairly easy to do and I could not find the kernel headers in the version that
was used for building the UML binary from the debian repository. vermagic mismatch when loading the driver \o/

Download and extract a Kernel archive or do a git checkout. I had to apply a mini-patch to the UML. The symbol of the funtions
that is creating the unix domain socket in the host filesystem is not exported. The driver wouldn't install because of
missing dependencies. So I just exported that symbol:

```
dgrafe@amd64-X2:~/git/kernel/staging$ git diff arch/um/os-Linux/user_syms.c
diff --git a/arch/um/os-Linux/user_syms.c b/arch/um/os-Linux/user_syms.c
index db4a034..b8b3564 100644
--- a/arch/um/os-Linux/user_syms.c
+++ b/arch/um/os-Linux/user_syms.c
@@ -118,3 +118,5 @@ EXPORT_SYMBOL(__guard);
 extern int __sprintf_chk(char *str, int flag, size_t strlen, const char *format);
 EXPORT_SYMBOL(__sprintf_chk);
 #endif
+
+EXPORT_SYMBOL_PROTO(umid_file_name);
```

Building the UML image is as easy as follows, just make sure you are including support for iomem when doing the config as it is unselected in the defconfig.


```
make ARCH=um x86_64_defconfig
make ARCH=um menuconfig
make ARCH=um -j
```

### Building the POC driver with the UML headers

Specify the target architecture and the location of the Kernel directory when building the driver for UML:
```
make KDIR=../../kernel/staging/ ARCH=um
make -C ../../kernel/staging/ M=/home/dgrafe/git/pci-mock/driver modules
make[1]: Entering directory '/home/dgrafe/git/kernel/staging'
  Building modules, stage 2.
  MODPOST 1 modules
make[1]: Leaving directory '/home/dgrafe/git/kernel/staging'
```

### Creating a minimal rootfs and iomem file

I chose to use multistrap for installing a minimal rootfs backed up by a file. You have to be root for these steps (And for these steps only):
```
dd if=/dev/zero of=/tmp/minimal.img bs=1M count=300
mkfs.ext3 /tmp/minimal.img
mount /tmp/minimal.img /mnt/image/ -o loop
mkdir /mnt/image/dev
mount -o bind /dev/ /mnt/image/dev/
multistrap -d /mnt/image/ -f ./multistrap/minimal.conf
umount /mnt/image/dev/
umount /mnt/image

dd if=/dev/zero of=/tmp/iomem.img bs=1M count=1
```

### Running the driver in the UML environment and mapping in a directory from the host

Run the vmlinux binary you have created befory with the following parameters:
* umid - Will give the virtual machine a name, makes locating the domain socket easier
* iomem - The location of the memory mapped file used for the PCI IO resource. It has to be tagged with the label "mock" because the POC driver ist expecting it
* ubd0 - The path to the rootfs image
* hostfs - The path to the directory on the host that should be mapped in
* init=/bin/bash - Just starts a shell, that will do for this test

Example:
```
./../kernel/staging/vmlinux umid=test iomem=mock,/tmp/iomem.img ubd0=/tmp/minimal.img hostfs=. init=/bin/bash
...
root@(none):/# mount -t proc proc /proc/
root@(none):/# mount -t hostfs hostfs /mnt/
root@(none):/# ls -l /mnt/
total 288
-rw-r--r-- 1 1000 1000    191 Jun 15 08:47 Makefile
-rw-r--r-- 1 1000 1000      0 Jun 15 14:19 Module.symvers
-rw-r--r-- 1 1000 1000   4255 Jun 15 12:33 mock-demo.c
-rw-r--r-- 1 1000 1000   3108 Jun 15 06:18 mock-demo.c.orig
-rw-r--r-- 1 1000 1000 128000 Jun 15 14:37 mock-demo.ko
-rw-r--r-- 1 1000 1000    454 Jun 15 14:37 mock-demo.mod.c
-rw-r--r-- 1 1000 1000  28472 Jun 15 14:37 mock-demo.mod.o
-rw-r--r-- 1 1000 1000 101040 Jun 15 14:37 mock-demo.o
-rw-r--r-- 1 1000 1000     53 Jun 15 14:37 modules.order
root@(none):/# insmod /mnt/mock-demo.ko 
mock_demo: loading out-of-tree module taints kernel.
UML: iomem mapped with size 1052672
```

The interrupt handler should now have been installed on interrupt line 12:
```
root@(none):/# mount -t proc proc /proc/
root@(none):/# cat /proc/interrupts 
           CPU0       
  0:       1632  SIGVTALRM  hr timer
  2:        110     SIGIO  console
  3:          0     SIGIO  console-write
  4:        394     SIGIO  ubd
  9:          0     SIGIO  mconsole
 10:          0     SIGIO  winch
 11:         99     SIGIO  write sigio
 12:          0     SIGIO  mock-demo
 14:          1     SIGIO  random
```

### Simulating the Link Status change

Run the RT8111 simulator from this project. You will have to specify the paths to the io mem file and the
interrupt trigger. Every invocation of the programm toggles the link status, i.e. it simulates the network
cable being removed or plugged in.

```
./sim -i /tmp/iomem.img -s ~/.uml/test/mock
 Changing Link Status to UP
./sim -i /tmp/iomem.img -s ~/.uml/test/mock
 Changing Link Status to DOWN
./sim -i /tmp/iomem.img -s ~/.uml/test/mock
 Changing Link Status to UP
./sim -i /tmp/iomem.img -s ~/.uml/test/mock
 Changing Link Status to DOWN
```

It triggers exactly the same response from the driver in the UML environment and four interrupts have been counted:
```
Link status changed to UP
Link status changed to DOWN
Link status changed to UP
Link status changed to DOWN

root@(none):/# cat /proc/interrupts 
           CPU0       
  0:       4461  SIGVTALRM  hr timer
  2:        130     SIGIO  console
  3:          0     SIGIO  console-write
  4:        395     SIGIO  ubd
  9:          0     SIGIO  mconsole
 10:          0     SIGIO  winch
 11:        116     SIGIO  write sigio
 12:          4     SIGIO  mock-demo
 14:          1     SIGIO  random
```
