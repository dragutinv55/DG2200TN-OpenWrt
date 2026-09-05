#!/usr/bin/python
# -*- coding: utf-8 -*-

#This tool expects data from the serial terminal in a format like this
#<RealTek>D 0x80300000 40
#80300000:       60C34A37        72783B1B        FA3DBF73        B5A5BE6F
#80300010:       4E01C8DF        AEF3DCFD        A796CC4F        6FB33EA6
#80300020:       4DE6CCBB        B43B07EF        EFD5FCB0        3732D4F7
#80300030:       7EDBB4F8        AD7DE946        2643A9B9        3DF31547
#80300040:       4B3BFC3B        CDEF9A97        938CFA51        FC448F43
#80300050:       E657FEF9        FA73DFCC        46FDD345        D5AB7959
#80300060:       6FAE73D4        9C8BDCC9        B2AB1E71        AF94EED5
#80300070:       552B5FBF        C6DFB259        6F6ECED1        8E7AB710
#80300080:       22FAEF57        6A5B303F        3FD57C76        5B614BDE
#80300090:       2FF4976B        6EB9E52F        D7BF8DBD        AB88B676

#Example command, backup 2MB of flash:
#  python2 cfetool.py --read=test.bin --addr=0xbfc00000 --size=0x200000 --block=0x10000

from __future__ import division
from optparse import OptionParser

import serial
import sys
import re

lineregex = re.compile(r'(?:[0-9A-F]{8})(?:[:])((?: [0-9A-F]{8}){1,4})')

def printf(string):
	sys.stdout.write(string)
	sys.stdout.flush()

def skip_prompt(ser):
	while ser.read(1):
		pass

def wait_prompt(ser):
	printf("Waiting for a prompt...")
	while True:
		ser.write('\r'.encode())
		if(ser.read(1)=='<' and ser.read(1)=='R' and ser.read(1)=='e' and ser.read(1)=='a' and ser.read(1)=='l' and ser.read(1)=='T' and ser.read(1)=='e' and ser.read(1)=='k' and ser.read(1)=='>'):
			skip_prompt(ser)
			printf(" OK\n")
			return

def memreadblock(ser, addr, size):
	skip_prompt(ser)
	ser.write("D %x %d\r" %(addr, size/4))
	buf=''
	m = False
	while not m:
		s = ser.readline().strip()
		m = lineregex.match(re.sub('\s+', ' ', s)) #trim repeated spaces to 1 and then apply lineregex
	while m:
		bytes = [chr(int(x[0:2],16))+chr(int(x[2:4],16))+chr(int(x[4:6],16))+chr(int(x[6:8],16)) for x in m.group(1)[1:].split(' ')]
		buf+=''.join(bytes)
		s = ser.readline().strip()
		m = lineregex.match(re.sub('\s+', ' ', s))
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
	ser = serial.Serial(options.serial, 38400, timeout=1)
	if options.read:
		memread(ser, options.read, int(options.addr, 0), int(options.size, 0), int(options.block, 0))
	return

if __name__ == '__main__':
	main()
