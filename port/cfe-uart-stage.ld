ENTRY(receiver)

SECTIONS
{
	. = 0x0e000000;
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
