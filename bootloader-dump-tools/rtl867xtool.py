#!/usr/bin/python
# -*- coding: utf-8 -*-

# This tool expects data from the serial terminal in a format like this
# <RTL867X>d 0x80000000 160
# 0x80000000: 10 00 00 FF 00 00 00 00 10 00 01 05 00 00 00 00 ................
# 0x80000010: 00 00 00 00 00 00 00 00 10 00 FF F9 00 00 00 00 ................
# 0x80000020: 10 00 FF F7 00 00 00 00 10 00 FF F5 00 00 00 00 ................
# 0x80000030: 10 00 FF F3 00 00 00 00 10 00 FF F1 00 00 00 00 ................
# 0x80000040: 10 00 FF EF 00 00 00 00 10 00 FF ED 00 00 00 00 ................
# 0x80000050: 10 00 FF EB 00 00 00 00 10 00 FF E9 00 00 00 00 ................
# 0x80000060: 10 00 FF E7 00 00 00 00 10 00 FF E5 00 00 00 00 ................
# 0x80000070: 10 00 FF E3 00 00 00 00 10 00 FF E1 00 00 00 00 ................
# 0x80000080: 10 00 FF DF 00 00 00 00 10 00 FF DD 00 00 00 00 ................
# 0x80000090: 10 00 FF DB 00 00 00 00 10 00 FF D9 00 00 00 00 ................
# <RTL867X>

# Example, dump 128MB, NAND flash?
# python2 rtl867xtool.py --read=test.bin --addr=0x80000000 --size=0x8000000 --block=0x10000 

from __future__ import division
from optparse import OptionParser

import serial
import sys
import re
import time

lineregex = re.compile(r'0x(?:[0-9A-F]{8})(?:[:])((?: [0-9A-F]{2}){1,16})')

def printf(string):
	sys.stdout.write(string)
	sys.stdout.flush()

def skip_prompt(ser):
	while ser.read(1):
		pass

def wait_prompt(ser):
	printf("Waiting for a prompt...")
	while True:
		ser.write("\x31") # Press 1 means entering boot mode
		ser.write("\x0D") # send carriage return
		if(ser.read(1) == '<' and ser.read(1) == 'R' and ser.read(1) == 'T' and ser.read(1) == 'L' and ser.read(1) == '8' and ser.read(1) == '6' and ser.read(1) == '7' and ser.read(1) == 'X' and ser.read(1) == '>'):
			time.sleep(1)
			ser.write("\x0D") # send carriage return to get a clean CLI
			skip_prompt(ser)
			printf(" OK\n")
			return

def memreadblock(ser, addr, size):
	skip_prompt(ser)
	ser.write("d 0x%x %d\r" %(addr, size))
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
		printf('%d%% (%d/%d)	Address: %s' %((cur_size / total_size) * 100, cur_size, total_size,hex(addr)))
		if size > block:
			memreadblock2file(ser, fd, addr, block)
			size -= block
			addr += block
		else:
			memreadblock2file(ser, fd, addr, size)
			size = 0
			printf("100%\n")
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
