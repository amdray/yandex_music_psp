	.set noreorder

#include "pspstub.s"

	STUB_START "KUBridge", 0x40090000, 0x00020005
	STUB_FUNC  0x9060F69D, kuKernelCall
	STUB_FUNC  0x24331850, kuKernelGetModel
	STUB_END
