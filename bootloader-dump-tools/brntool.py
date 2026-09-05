#!/usr/bin/python
# -*- coding: utf-8 -*-

#------------- Bootloader dump sample:   ----------------------#
#[VR9 Boot]:r
#
#Enter the Start Address to Read....0x			B0000000
#Data Length is (1) 4 Bytes (2) 2 Bytes (3) 1 Byte... 	3
#Enter the Count to Read....(Maximun 10000)		160
#
#----------------------------------------------------------
# Address   00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
#----------------------------------------------------------
#0xB0000000 10 00 00 0B 00 00 00 00 00 00 00 00 00 00 00 00 
#0xB0000010 68 8C 68 8C 00 00 00 00 31 2E 31 2E 30 00 00 00 
#0xB0000020 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#0xB0000030 40 80 90 00 40 80 98 00 40 80 68 00 40 1B 78 00 
#0xB0000040 3C 08 00 FF 35 08 FF 00 03 68 D8 24 3C 08 00 01 
#0xB0000050 35 08 95 00 17 68 00 19 00 00 00 00 40 08 80 00 
#0xB0000060 3C 09 80 00 35 29 FF FF 01 09 40 24 3C 09 36 04 
#0xB0000070 01 09 40 25 00 00 00 00 40 88 80 00 00 00 00 40 
#0xB0000080 00 00 00 40 00 00 00 40 00 00 00 C0 40 08 60 00 
#0xB0000090 3C 09 FF FC 35 29 FF FF 01 09 40 24 24 09 00 00 
#
#[VR9 Boot]:
#-------------------------------------------------------------#

#Keep python2 working
from __future__ import division #1/2 = float, 1//2 = integer
from __future__ import print_function #print("blah", file=whatever)
#Keep python2 working end
from optparse import OptionParser
import serial
import sys
#import struct
import re
lineregex = re.compile(r'0x(?:[0-9A-F]{8})((?: [0-9A-F]{2}){1,16})')
def get2menu(ser,verbose):
	if verbose:
		print("Waiting for a prompt...", file=sys.stderr)
	while True:
		ser.write("   !".encode())
		if(ser.read(1)==b']' and ser.read(1)==b':'):
			while ser.read(256):
				pass
			if verbose:
				print("Ok.", file=sys.stderr)
			return
def memreadblock(ser,addr,size):
	while ser.read(1):
		pass
	ser.write('r'.encode())
	while not (ser.read(1)==b'0' and ser.read(1)==b'x'):
		pass
	ser.write(("%x"%addr).encode())
	ser.write('\r'.encode())
	while not (ser.read(1)==b'.' and ser.read(1)==b'.' and ser.read(1)==b'.'):
		pass
	ser.write('3'.encode())
	while not ser.read(1)==b')':
		pass
	ser.write(str(size).encode())
	ser.write('\r'.encode())
	buf=b''
	m = False
	while not m:
		m = lineregex.match(ser.readline().decode().strip())
	while m:
		if sys.version_info >= (3, 0):
			buf+=bytes.fromhex(m.group(1))
		else:
			buf+=''.join([chr(int(x, 16)) for x in m.group(1)[1:].split(' ')])
		m = lineregex.match(ser.readline().decode().strip())
	return buf
def memreadblock2file(ser,fd,addr,size):
	while True:
		buf=memreadblock(ser,addr,size)
		if len(buf)==size:
			break
		sys.stderr.write('!')
	fd.write(buf)
	return
def memread(ser,path,addr,size,verbose):
	#bs=1024
	bs=10000 #10000 is usually the maximum size for an hexdump on brnboot.
	get2menu(ser,verbose)
	if path == "-":
		# get sys.stdout in Python 2 or sys.stdout.buffer in Python 3
		fd=getattr(sys.stdout, 'buffer', sys.stdout)
	else:
		fd=open(path,"wb")
	while 0<size:
		if size>bs:
			memreadblock2file(ser,fd,addr,bs)
			size-=bs
			addr+=bs
			print("Addr: " + hex(addr), file=sys.stderr)
			print("Size: " + str(size), file=sys.stderr)
		else:
			memreadblock2file(ser,fd,addr,size)
			size=0
	fd.close()
	return
def main():
	optparser = OptionParser("usage: %prog [options]",version="%prog 0.1")
	optparser.add_option("--verbose", action="store_true", dest="verbose", help="be verbose", default=False)
	optparser.add_option("--serial", dest="serial", help="specify serial port", default="/dev/ttyUSB0", metavar="dev")
	optparser.add_option("--read", dest="read", help="read mem to file", metavar="path")
	#optparser.add_option("--write", dest="write",help="write mem from file", metavar="path")
	optparser.add_option("--addr", dest="addr",help="mem address", metavar="addr")
	optparser.add_option("--size", dest="size",help="size to copy", metavar="bytes")
	(options, args) = optparser.parse_args()
	if len(args) != 0:
		optparser.error("incorrect number of arguments")
	ser = serial.Serial(options.serial, 115200, timeout=1)
	if options.read:
		memread(ser,options.read,int(options.addr,0),int(options.size,0),options.verbose)
	return
if __name__ == '__main__':
	main()
