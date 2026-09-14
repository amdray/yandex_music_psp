#ifndef SERVICES_SYSTEMCTRL_RNG_H
#define SERVICES_SYSTEMCTRL_RNG_H

#include <stddef.h>

/* mbedTLS-compatible hardware RNG backed by ARK-4 SystemCtrlForUser. */
int systemctrl_rng(void *context, unsigned char *output, size_t output_len);

/* One-shot hardware diagnostic. */
void systemctrl_rng_probe(void);

#endif
