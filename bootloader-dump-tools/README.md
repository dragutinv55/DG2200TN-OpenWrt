# bootloader-dump tools
Collection of tools for dumping the memory or backing up the flash chip using the **memory read** native  
command present in bootloaders from some devices (not all) like routers.

Usually you access to the bootloader command line using an UART adapter, and then if the command is  
available dump small portions of the memory in plain text, these tools automate the process for getting  
a full backup in binary format. It deals through the serial port with the bootloader by sending the command  
**memory read** and captures the output dumping it into a binary file.

# Prerequisites
- All scripts require Python 2 to be installed.
- "pyserial" package needs to be installed via pip.
- If using Windows, change the source serial from "/dev/ttyUSB0" to "COM3" or whtever COM number is attached to your UART adapter in Device Manager.
- If using multiple Python versions, replace "python2" command with "py -2".


## brntool
This tool can, so far, given a serial port connected to a device with **brnboot / amazonboot**, dump its flash into a file.  
Homepage: https://github.com/rvalles/brntool  
  
**Example:**  
`python3 brntool.py --read=AR4518PW_whole.dump --addr=0xB0000000 --verbose --size=0x400000`  

   --addr: Memory Address  
   --size: Memory Size  
   --block: Buffer size (Default: 10240 -> 10Kb)  
   
## cfetool
This tool can dump the flash of a device with CFE bootloader into a file.  
It's compatible with all CFE bootloaders with "dm" command usually found in **BCM63xx SoCs**.  
  
**Example:**  
`python2 cfetool.py --read=test.bin --addr=0xB8000000 --size=0x20000 --block=0x10000`  
  
--addr: Memory Address  
--size: Memory Size  
--block: Buffer size (Default: 10240 -> 10Kb)  
  
**Zyxel variants:**  
zyx1tool.py, zyx2tool.py

## cfenand
This tool can dump the NAND flash of a device with CFE bootloader into a file.  
It's compatible with all CFE bootloaders with "dn" command usually found in **BCM63xx SoCs**.  
Homepage: https://github.com/Depau/bcm-cfedump
  
**Example:**  
`python -m cfenand -D /dev/ttyUSB0 -O nand.bin -t 0.05 nand`  
  
Tested with a BCM63167 Sercomm router (128MB flash).

## cfenandzyx
For broadcom NAND devices with a CFE bootloader modded by Zyxel with the **ATDF** command available. Tested on Mitrastar GPT-2541GNAC  

**Example:**  
  * enable the ATDF command: open the console with minicom and execute:   
`ATEN 1 10F0A563`
  * dump the flash: close minicom and execute on the computer:  
`python cfenandzyx.py --verbose --blkn 0 --size 0x8000000 --read mitrastardump.bin`  

   --size: Memory Size  
   --blkn: flash block position (first is 0)  

## rt63365tool
For **Ralink RT63365** (Trendchip) based SoCs running the tcboot bootloader. Tested on Huawei HG532s  
  
**Example:**  
`python2 rt63365tool.py --read=test.bin --addr=0xB0000000 --size=0x800000 --block=0x10000`  
  
--addr: Memory Address  
--size: Memory Size  
--block: Buffer size  

## en751221tool
For **Econet EN751221** based SoCs running the tcboot bootloader. Tested on ZTE H367A

**Example:**  
  * open minicom in the bootloader CLI execute:  
`readflash 80020000 0 1a00000`  
  * close minicom, at the pc execute:  
`python2 rt63365tool.py --read=dump1.bin --addr=0x80020000 --size=0x1a00000 --block=0x10000`  

## rtl8186tool

This tool can dump the flash memory of a **Realtek RTL8186** based device running the btcode bootloader  

**Example:**  
`python2 rtl8186tool.py --read=test.bin --addr=0xbfc00000 --size=0x200000 --block=0x10000`  
  
   --addr: Memory Address  
   --size: Memory Size  
   --block: Buffer size (Default: 10240 -> 10Kb)  

**Note:** the default baud rate used by the tool is 38400 bps  

   
## rtl867xtool

This tool was tested on a **Realtek RTL8676** based device (ZTE H298N)  

**Example:**  
`python2 rtl867xtool.py --read=test.bin --addr=0x80000000 --size=0x8000000 --block=0x10000`  
  
   --addr: Memory Address  
   --size: Memory Size  
   --block: Buffer size (Default: 10240 -> 10Kb)  


---

All tools are based/inspired on the original brntool (@rvalles): Homepage: https://github.com/rvalles/brntool
