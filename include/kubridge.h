#ifndef YM_KUBRIDGE_STUB_H
#define YM_KUBRIDGE_STUB_H

// Заглушка для kubridge (ARK CFW). Реальный kuKernelGetModel — из kubridge.
// Здесь только то, что использует ui_screen_memory.c.
#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int kuKernelGetModel(void);

#ifdef __cplusplus
}
#endif

#endif
