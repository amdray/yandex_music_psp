#include "services/systemctrl_rng.h"

#include <stdint.h>
#include <string.h>

#include "core/logger.h"

/* ARK-4 SystemCtrlForUser export imported by systemctrl_rng_stub.S. */
extern unsigned int sctrlKernelRand(void);

int systemctrl_rng(void *context, unsigned char *output, size_t output_len)
{
    size_t offset = 0;
    (void)context;

    if (!output && output_len != 0) return -1;

    while (offset < output_len) {
        uint32_t word = sctrlKernelRand();
        size_t count = output_len - offset;
        if (count > sizeof(word)) count = sizeof(word);
        memcpy(output + offset, &word, count);
        offset += count;
    }
    return 0;
}

void systemctrl_rng_probe(void)
{
    unsigned int values[5];
    int all_zero = 1;
    int all_equal = 1;
    int i;

    logLine("rng: ARK SystemCtrlForUser sctrlKernelRand probe begin\n");
    logger_flush();

    for (i = 0; i < 5; i++) {
        values[i] = sctrlKernelRand();
        if (values[i] != 0) all_zero = 0;
        if (i > 0 && values[i] != values[0]) all_equal = 0;
    }

    logLine("rng: ARK direct values=%08X %08X %08X %08X %08X\n",
            values[0], values[1], values[2], values[3], values[4]);
    if (all_zero) {
        logLine("rng: ARK direct WARNING all values are zero\n");
    } else if (all_equal) {
        logLine("rng: ARK direct WARNING all values are identical\n");
    } else {
        logLine("rng: ARK direct variability check OK\n");
    }
    logger_flush();
}
