ARCH:=arm
SUBTARGET:=armv7
BOARDNAME:=32-bit (armv7) machines
CPU_TYPE:=cortex-a9
FEATURES:=$(filter-out fpu,$(FEATURES))
KERNELNAME:=zImage

define Target/Description
  Build images for $(BOARDNAME)
endef
