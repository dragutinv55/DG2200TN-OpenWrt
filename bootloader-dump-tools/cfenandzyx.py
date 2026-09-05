#!/usr/bin/python
# -*- coding: utf-8 -*-

#enable the ATDF command:
#CFE> ATEN 1 10F0A563
# 
#dump the block 0:
#CFE> ATDF 0
#
#10 00 02 81 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
#00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
# -----         trimmed lines             -----
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
#ff ff ff ff ff ff ff ff 00 00 00 00 00 01 ca ff 
#*** command status = 0
#CFE>

# example, dump 128MB NAND flash:
# python cfenandzyx.py --verbose --blkn 0 --size 0x8000000 --read mitrastardump.bin

#Keep python2 working
from __future__ import division #1/2 = float, 1//2 = integer
from __future__ import print_function #print("blah", file=whatever)
#Keep python2 working end
from optparse import OptionParser
import serial
import sys
#import struct
import re
lineregex = re.compile(r'((?:[0-9a-f]{2} ){16})')
def get2menu(ser,verbose):
	if verbose:
		print("Waiting for a prompt...", file=sys.stderr)
	while True:
		ser.write('\r'.encode())
		if(ser.read(1)==b'C' and ser.read(1)==b'F' and ser.read(1)==b'E' and ser.read(1)==b'>'):
			while ser.read(256):
				pass
			if verbose:
				print("Ok.", file=sys.stderr)
			return

def memreadblock(ser,blkn):
	while ser.read(1):
		pass
	ser.write(("ATDF %d"%blkn).encode())
	ser.write('\r'.encode())
	buf = b''
	m = False
	while not m:
		m = lineregex.match(ser.readline().decode())
	while m:
		#print(m.groups())
		if sys.version_info >= (3, 0):
			buf += bytes.fromhex(m.group(1))
		else:
			buf += ''.join([chr(int(x, 16)) for x in m.group(1)[1:].split(' ')])
		m = lineregex.match(ser.readline().decode())
	return buf

def memreadblock2file(ser,fd,blkn,size):
	while True:
		buf = memreadblock(ser,blkn)
		if len(buf) == size:
			break
		sys.stderr.write('!')
	fd.write(buf)
	return

def memread(ser,path,blkn,size,verbose):
	#block size should be 131072 = 128kB = 0x20000
	bs = 131072
	get2menu(ser,verbose)
	if path == "-":
		# get sys.stdout in Python 2 or sys.stdout.buffer in Python 3
		fd = getattr(sys.stdout, 'buffer', sys.stdout)
	else:
		fd = open(path,"wb")
	while size > 0:
		if size > bs:
			print("Block: " + str(blkn), file=sys.stderr)
			print("Size: " + str(size), file=sys.stderr)
			memreadblock2file(ser,fd,blkn,bs)
			size -= bs
			blkn += 1
		else:
			print("Block: " + str(blkn), file=sys.stderr)
			print("Size: " + str(size), file=sys.stderr)
			memreadblock2file(ser,fd,blkn,size)
			size = 0
	fd.close()
	return

def main():
	optparser = OptionParser("usage: %prog [options]",version="%prog 0.1")
	optparser.add_option("--verbose", action="store_true", dest="verbose", help="be verbose", default=False)
	optparser.add_option("--serial", dest="serial", help="specify serial port", default="/dev/ttyUSB0", metavar="dev")
	optparser.add_option("--read", dest="read", help="read mem to file", metavar="path")
	optparser.add_option("--blkn", dest="blkn",help="block position", metavar="blkn")
	optparser.add_option("--size", dest="size",help="size to copy", metavar="bytes")
	(options, args) = optparser.parse_args()
	if len(args) != 0:
		optparser.error("incorrect number of arguments")
	ser = serial.Serial(options.serial, 115200, timeout=1)
	if options.read:
		memread(ser,options.read,int(options.blkn,0),int(options.size,0),options.verbose)
	return
if __name__ == '__main__':
	main()
