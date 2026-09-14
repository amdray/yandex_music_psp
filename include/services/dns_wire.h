#ifndef YM_SERVICES_DNS_WIRE_H
#define YM_SERVICES_DNS_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define DNS_WIRE_MAX_NAME   254
#define DNS_WIRE_MAX_PACKET 2048

#define DNS_WIRE_OK          0
#define DNS_WIRE_CNAME_ONLY  1
#define DNS_WIRE_TRUNCATED   2
#define DNS_WIRE_NXDOMAIN    3
#define DNS_WIRE_RETRYABLE   4
#define DNS_WIRE_INVALID    -1

typedef struct DnsWireResult {
    uint32_t ipv4_be;
    uint32_t ttl;
    char canonical_name[DNS_WIRE_MAX_NAME];
} DnsWireResult;

int dns_wire_build_query(const char *hostname, uint16_t id,
                         unsigned char *packet, size_t capacity,
                         size_t *out_size);
int dns_wire_parse_response(const unsigned char *packet, size_t packet_size,
                            uint16_t id, const char *hostname,
                            DnsWireResult *out);

#endif
