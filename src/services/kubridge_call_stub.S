	.set noreorder

#include "pspstub.s"

	STUB_START "KUBridge", 0x40090000, 0x00010005
	STUB_FUNC  0x9060F69D, kuKernelCall
	STUB_END
