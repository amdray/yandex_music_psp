/* ARK-4's direct hardware RNG wrapper (SystemCtrlForUser).
 * NID and library name come from ARK-4's generated import library. */

	.set noreorder

#include "pspstub.s"

	STUB_START "SystemCtrlForUser", 0x40090000, 0x00020005
	STUB_FUNC  0x159AF5CC, sctrlHENFindFunction
	STUB_FUNC  0xB364FBB4, sctrlKernelRand
	STUB_END
