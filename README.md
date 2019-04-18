# TI AM3358 DMTimer usage example
This dummy driver toggles BeagleBone Black's P9_27 pin.
Toggle happens in simple non-threaded irq handler.

## Why?
I made this driver to compare accuracy of dmtimer and hrtimer framework.
dmtimer appears to be 10 times more precise and it's irq latency +/-10usec unstable, when hrtimer shoots with 100 usec instability.

Eventually I didn't need it, maybe it helps somebody!

## BBB distro used
To test driver I used AMAZING debian distro found here:
https://github.com/RobertCNelson/ti-linux-kernel-dev

Scripts automatically clones kernel src, patches it and builds zImage, modules and dtbs.

My version was
4.14.94-ti-r93

if yours doesn't match and you encounter problems try:
git checkout v4.14.94

## DMTimer kernel API
The only example I found:
https://e2e.ti.com/support/processors/f/791/t/415079

in latest kernels they'd completely changed the way of exporting methods,
started to pack them into struct and save in platform data.

An example of extracting this struct can be found in
/drivers/pwm/pwm-omap-dmtimer.c
(and in this driver)

## Device tree and in-tree build
I put driver code under root directory ti-linux-kernel-dev/dmtimer_drv.

After building kernel according to RobertCNelson instructions kernel source is 
cloned into ti-linux-kernel-dev/KERNEL, so driver code is next to kernel source and
can be built like this:
username> ~/ti-linux-kernel-dev/KERNEL$ make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=../dmtimer_drv

For driver to be built and appear under misc drivers menuconfig tab some changes are to be made to
kernel build system and dts files.

All these changes can be found in intree.patch file.

After this kernel, modules and dtbs must be recompiled.

After recompiling dts files, newly compiled /arch/arm/boot/dts/am335x-boneblack-uboot-univ.dtb
must be copied to beaglebone's /boot/dtbs/4.14.94-ti-r93/ directory.

After reboot BBB with new dtb file dmtimer_drv.ko can be simply insmod'ed.

# Contributors
me me me
	(c) Agent Smith
