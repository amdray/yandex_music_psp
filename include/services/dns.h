#ifndef YM_SERVICES_DNS_H
#define YM_SERVICES_DNS_H

#include <netinet/in.h>

typedef int (*DnsCancelFn)(void *ctx);

// Direct DNS client lifecycle. Queries use the DNS servers of the active
// APCTL profile and never call the process-global PSP resolver service.
int dns_init(void);
int dns_shutdown(void);
int dns_resolve(const char *host, struct in_addr *out_addr, int *out_cacheable,
                DnsCancelFn cancel, void *cancel_ctx);
void dns_invalidate(const char *host, const struct in_addr *addr);

#define DNS_ERR_INVALID  (-1)
#define DNS_ERR_OFFLINE  (-2)
#define DNS_ERR_LOOKUP   (-3)
#define DNS_ERR_TIMEOUT  (-4)
#define DNS_ERR_CANCELLED (-5)

#endif /* YM_SERVICES_DNS_H */
