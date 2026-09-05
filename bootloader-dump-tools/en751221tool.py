#!/usr/bin/python
# -*- coding: utf-8 -*-

# This tool expects data from the serial terminal in a format like this
# bldr> dump b0000000 a0                                                                                                               
# b0000000  0b f0 00 0a.00 00 00 00.00 00 00 00.00 00 00 00  |................|                                                        
# b0000010  00 00 00 00.00 00 00 00.00 00 00 00.00 00 00 00  |................|                                                        
# b0000020  00 00 00 00.00 00 00 00.40 80 90 00.40 80 98 00  |........@...@...|                                                        
# b0000030  40 1a 60 00.24 1b ff e6.03 5b d0 24.40 9a 60 00  |@.`.$....[.$@.`.|                                                        
# b0000040  3c 1a 00 80.40 9a 68 00.0f f0 00 a2.00 00 00 00  |<...@.h.........|                                                        
# b0000050  0f f0 01 60.00 00 00 00.3c 1a bf b0.8f 5b 00 64  |...`....<....[.d|                                                        
# b0000060  00 00 00 00.00 1b dc 02.23 7b ff fd.07 60 00 05  |........#{...`..|                                                        
# b0000070  00 00 00 00.0f f0 01 e1.00 00 00 00.0b f0 00 23  |...............#|                                                        
# b0000080  00 00 00 00.0f f0 03 7e.00 00 00 00.0f f0 01 7c  |.......~.......||                                                        
# b0000090  00 00 00 00.3c 02 bf bf.34 42 02 00.24 04 00 00  |....<...4B..$...|                                                        
# bldr> 

#Example command, backup 128MB of flash, 4 dumps: 
#1st dump
#open minicom in the bootloader CLI execute:
#readflash 80020000 0 1a00000
#close minicom, at the pc execute:
#python2 en751221tool.py --read=dump1.bin --addr=0x80020000 --size=0x1a00000 --block=0x10000
#
#2nd dump
#readflash 80020000 1a00000 1800000
#python2 en751221tool.py --read=dump2.bin --addr=0x80020000 --size=0x1800000 --block=0x10000
#
#3th dump
#readflash 80020000 3200000 3e00000
#python2 en751221tool.py --read=dump3.bin --addr=0x80020000 --size=0x3e00000 --block=0x10000
#
#4th dump
#readflash 80020000 7000000 1000000
#python2 en751221tool.py --read=dump4.bin --addr=0x80020000 --size=0x1000000 --block=0x10000

from __future__ import division
from optparse import OptionParser

import serial
import sys
import re
import time

lineregex = re.compile(r'(?:[0-9a-f]{8} )((?:[ |\\.][0-9a-f]{2}){1,16})')

def printf(string):
	sys.stdout.write(string)
	sys.stdout.flush()

def skip_prompt(ser):
	while ser.read(1):
		pass

def wait_prompt(ser):
	printf("Waiting for a prompt...")
	ser.flush()
	while True:
		ser.write("\x31") # Press 1 means entering boot mode
		ser.write("\x0D") #send carriage return
		if(ser.read(1) == 'b' and ser.read(1) == 'l' and ser.read(1) == 'd' and ser.read(1) == 'r' and ser.read(1) == '>'):
			ser.write("\x0D") #send carriage return to get a CLI
			time.sleep(1)
			skip_prompt(ser)
			printf(" OK\n")
			return

def memreadblock(ser, addr, size):
	skip_prompt(ser)
	ser.write("dump %x %x\r" %(addr, size))
	buf=''
	m = False
	while not m:
		line = ser.readline().strip().replace(".", " ", 3)
		m = lineregex.match(line)
	while m:
		bytes = [chr(int(x, 16)) for x in m.group(1)[1:].split(' ')]
		buf+=''.join(bytes)
		line = ser.readline().strip().replace(".", " ", 3)
		m = lineregex.match(line)
	return buf

def memreadblock2file(ser, fd, addr, size):
	while True:
		buf = memreadblock(ser, addr, size)
		if len(buf) == size:
			break
		printf(' [!]\n')
	printf(' [.]\n')
	fd.write(buf)
	return

def memread(ser, path, addr, size, block):
	wait_prompt(ser)
	total_size = size
	fd = open(path, "wb")
	while size > 0:
		cur_size = (total_size - size)
		printf('%d%% (%d/%d)' %((cur_size / total_size) * 100, cur_size, total_size))
		if size > block:
			memreadblock2file(ser, fd, addr, block)
			size -= block
			addr += block
		else:
			memreadblock2file(ser, fd, addr, size)
			size = 0
	fd.close()
	return

def main():
	optparser = OptionParser("usage: %prog [options]",version="%prog 0.1")
	optparser.add_option("--block", dest="block", help="buffer block size", default="10240",metavar="block")
	optparser.add_option("--serial", dest="serial", help="specify serial port", default="/dev/ttyUSB0", metavar="dev")
	optparser.add_option("--read", dest="read", help="read mem to file", metavar="path")
	optparser.add_option("--addr", dest="addr",help="mem address", metavar="addr")
	optparser.add_option("--size", dest="size",help="size to copy", metavar="bytes")
	(options, args) = optparser.parse_args()
	if len(args) != 0:
		optparser.error("incorrect number of arguments")
	ser = serial.Serial(options.serial, 115200, timeout=1)
	if options.read:
		memread(ser, options.read, int(options.addr, 0), int(options.size, 0), int(options.block, 0))
	return

if __name__ == '__main__':
	main()
