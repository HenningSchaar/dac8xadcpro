# dac8xadcpro

Driver for the dac8xadcpro sound card prototype from HiFiBerry for Raspberry Pi 5. Tested on kernel version 6.12.47+rpt-rpi-2712.

## Build and Install

1. run `make`
2. run `sudo ./build.sh`
3. If building for the first time 
    - add `dtoverlay=hifiberry-dac8xadcpro-overlay` to `/boot/firmware/config.txt`
    - run `sudo insmod /lib/modules/$(uname -r)/kernel/sound/soc/bcm/hifiberry-dac8xadcpro.ko"`
4. reboot for changes to apply

## Dependencies
- Raspberry Pi kernel headers, for example: `sudo apt-get install linux-headers-6.12.47+rpt-common-rpi`

## Comparison

To compare clocking schemes switch to consumer driver branch: `git checkout consumer-driver` and follow the build and install instructions again.