ENTRY(receiver)

SECTIONS
{
	. = 0x00f00584;
	.text : {
		*(.text.start)
		*(.text*)
		*(.rodata*)
	}

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.ARM.attributes)
	}
}
