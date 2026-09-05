#!/usr/bin/python
# -*- coding: utf-8 -*-

# This tool expects data from the serial terminal in a format like this
# CFE> dm b8020000 160     
# b8020000: 36 00 00 00 42 72 6f 61 64 63 6f 6d 20 43 6f 72    6...Broadcom Cor
# b8020010: 70 6f 72 61 74 69 6f 00 76 65 72 2e 20 32 2e 30    poratio.ver. 2.0
# b8020020: 00 00 00 00 00 00 36 33 36 38 00 00 39 36 33 36    ......6368..9636
# b8020030: 38 4d 56 57 47 00 00 00 00 00 00 00 31 00 34 30    8MVWG.......1.40
# b8020040: 36 32 39 38 30 00 00 00 30 00 00 00 00 00 00 00    62980...0.......
# b8020050: 00 00 00 00 30 00 00 00 00 00 00 00 00 00 33 32    ....0.........32
# b8020060: 31 37 31 36 32 34 39 36 00 00 32 36 30 31 35 32    17162496..260152
# b8020070: 38 00 00 00 33 32 31 37 31 36 32 34 39 36 00 00    8...3217162496..
# b8020080: 31 34 36 31 34 35 32 00 00 00 32 00 00 00 00 00    1461452...2.....
# b8020090: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00    ................
# 
# *** command status = 0
# CFE> 

from __future__ import division
from optparse import OptionParser

import serial
import sys
import re

lineregex = re.compile(r'(?:[0-9a-f]{8})(?:[:])((?: [0-9a-f]{2}){1,16})')
#lineregex = re.compile(r'(?:[0-9a-f]{8})(?:[:])((?: [0-9a-f]{2}){1,16})(?:\s{4})(?:.{16})')

def printf(string):
	sys.stdout.write(string)
	sys.stdout.flush()

def skip_prompt(ser):
	while ser.read(1):
		pass

def wait_prompt(ser):
	printf("Waiting for a prompt...")
	while True:
		ser.write("\x03")
		if(ser.read(1) == 'C' and ser.read(1) == 'F' and ser.read(1) == 'E' and ser.read(1) == '>'):
			skip_prompt(ser)
			printf(" OK\n")
			return

def memreadblock(ser, addr, size):
	skip_prompt(ser)
	ser.write("dm %x %d\r" %(addr, size))
	buf=''
	m = False
	while not m:
		m = lineregex.match(ser.readline().strip())
	while m:
		bytes = [chr(int(x, 16)) for x in m.group(1)[1:].split(' ')]
		buf+=''.join(bytes)
		m = lineregex.match(ser.readline().strip())
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
