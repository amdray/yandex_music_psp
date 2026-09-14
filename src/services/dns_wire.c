#include "services/dns_wire.h"

#include <string.h>

#define DNS_MAX_ANSWERS 32
#define DNS_MAX_TOTAL_RR 64
#define DNS_MAX_CNAME_DEPTH 8
#define DNS_MAX_POINTER_JUMPS 32

typedef struct ParsedAnswer {
    char owner[DNS_WIRE_MAX_NAME];
    char target[DNS_WIRE_MAX_NAME];
    uint32_t address;
    uint32_t ttl;
    uint16_t type;
} ParsedAnswer;

static uint16_t read_be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static int ascii_fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int name_equal(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t i;
    while (alen > 0 && a[alen - 1] == '.') --alen;
    while (blen > 0 && b[blen - 1] == '.') --blen;
    if (alen != blen) return 0;
    for (i = 0; i < alen; ++i)
        if (ascii_fold((unsigned char)a[i]) !=
            ascii_fold((unsigned char)b[i])) return 0;
    return 1;
}

static int hostname_valid(const char *name, size_t *out_len)
{
    size_t i;
    size_t label = 0;
    size_t len;
    if (!name) return 0;
    len = strlen(name);
    if (len > 0 && name[len - 1] == '.') --len;
    if (len == 0 || len > 253) return 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == '.') {
            if (label == 0 || label > 63) return 0;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) return 0;
            ++label;
        }
    }
    if (label == 0 || label > 63) return 0;
    if (out_len) *out_len = len;
    return 1;
}

static int decode_name(const unsigned char *packet, size_t size,
                       size_t *cursor, char *out, size_t out_size)
{
    size_t pos;
    size_t resume = 0;
    size_t out_len = 0;
    size_t visited[DNS_MAX_POINTER_JUMPS];
    int visited_count = 0;
    int jumped = 0;
    if (!packet || !cursor || !out || out_size == 0 || *cursor >= size)
        return -1;
    pos = *cursor;
    for (;;) {
        unsigned int octet;
        if (pos >= size) return -1;
        octet = packet[pos];
        if ((octet & 0xC0U) == 0xC0U) {
            size_t target;
            size_t pointer_pos = pos;
            int i;
            if (pos + 1 >= size) return -1;
            target = ((size_t)(octet & 0x3FU) << 8) | packet[pos + 1];
            if (target >= pointer_pos || target >= size ||
                visited_count >= DNS_MAX_POINTER_JUMPS)
                return -1;
            for (i = 0; i < visited_count; ++i)
                if (visited[i] == target) return -1;
            visited[visited_count++] = target;
            if (!jumped) resume = pos + 2;
            jumped = 1;
            pos = target;
            continue;
        }
        if ((octet & 0xC0U) != 0) return -1;
        ++pos;
        if (octet == 0) break;
        if (octet > 63 || pos + octet > size) return -1;
        if (out_len != 0) {
            if (out_len + 1 >= out_size) return -1;
            out[out_len++] = '.';
        }
        if (out_len + octet >= out_size || out_len + octet > 253)
            return -1;
        {
            unsigned int label_index;
            for (label_index = 0; label_index < octet; ++label_index) {
                unsigned char c = packet[pos + label_index];
                if (!((c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-'))
                    return -1;
            }
        }
        memcpy(out + out_len, packet + pos, octet);
        out_len += octet;
        pos += octet;
    }
    out[out_len] = '\0';
    *cursor = jumped ? resume : pos;
    return 0;
}

int dns_wire_build_query(const char *hostname, uint16_t id,
                         unsigned char *packet, size_t capacity,
                         size_t *out_size)
{
    size_t len;
    size_t start = 0;
    size_t pos = 12;
    size_t i;
    if (!packet || !out_size || !hostname_valid(hostname, &len)) return -1;
    if (capacity < len + 18) return -1;
    memset(packet, 0, 12);
    write_be16(packet, id);
    write_be16(packet + 2, 0x0100U);
    write_be16(packet + 4, 1);
    for (i = 0; i <= len; ++i) {
        if (i == len || hostname[i] == '.') {
            size_t label_len = i - start;
            packet[pos++] = (unsigned char)label_len;
            memcpy(packet + pos, hostname + start, label_len);
            pos += label_len;
            start = i + 1;
        }
    }
    packet[pos++] = 0;
    write_be16(packet + pos, 1); pos += 2;
    write_be16(packet + pos, 1); pos += 2;
    *out_size = pos;
    return 0;
}

int dns_wire_parse_response(const unsigned char *packet, size_t size,
                            uint16_t id, const char *hostname,
                            DnsWireResult *out)
{
    ParsedAnswer answers[DNS_MAX_ANSWERS];
    char question[DNS_WIRE_MAX_NAME];
    char current[DNS_WIRE_MAX_NAME];
    char seen[DNS_MAX_CNAME_DEPTH][DNS_WIRE_MAX_NAME];
    uint16_t flags, qd, an, ns, ar;
    size_t cursor = 12;
    unsigned int stored = 0;
    unsigned int total;
    unsigned int i;
    uint32_t chain_ttl = 0xFFFFFFFFU;
    int depth;
    if (!packet || !hostname || !out || size < 12 || size > DNS_WIRE_MAX_PACKET)
        return DNS_WIRE_INVALID;
    if (!hostname_valid(hostname, NULL)) return DNS_WIRE_INVALID;
    memset(out, 0, sizeof(*out));
    flags = read_be16(packet + 2);
    qd = read_be16(packet + 4);
    an = read_be16(packet + 6);
    ns = read_be16(packet + 8);
    ar = read_be16(packet + 10);
    if (read_be16(packet) != id || (flags & 0x8000U) == 0 ||
        (flags & 0x7800U) != 0 || (flags & 0x0040U) != 0 || qd != 1)
        return DNS_WIRE_INVALID;
    total = (unsigned int)an + ns + ar;
    if (an > DNS_MAX_ANSWERS || total > DNS_MAX_TOTAL_RR)
        return DNS_WIRE_INVALID;
    if (decode_name(packet, size, &cursor, question, sizeof(question)) < 0 ||
        cursor + 4 > size || !name_equal(question, hostname) ||
        read_be16(packet + cursor) != 1 ||
        read_be16(packet + cursor + 2) != 1)
        return DNS_WIRE_INVALID;
    cursor += 4;
    if (flags & 0x0200U) return DNS_WIRE_TRUNCATED;
    if ((flags & 0x000FU) == 3) return DNS_WIRE_NXDOMAIN;
    if ((flags & 0x000FU) != 0) return DNS_WIRE_RETRYABLE;
    memset(answers, 0, sizeof(answers));
    for (i = 0; i < total; ++i) {
        char owner[DNS_WIRE_MAX_NAME];
        uint16_t type, klass, rdlen;
        uint32_t ttl;
        size_t rdata;
        if (decode_name(packet, size, &cursor, owner, sizeof(owner)) < 0 ||
            cursor + 10 > size) return DNS_WIRE_INVALID;
        type = read_be16(packet + cursor);
        klass = read_be16(packet + cursor + 2);
        ttl = read_be32(packet + cursor + 4);
        rdlen = read_be16(packet + cursor + 8);
        cursor += 10;
        rdata = cursor;
        if (cursor + rdlen > size) return DNS_WIRE_INVALID;
        if (i < an && klass == 1 && stored < DNS_MAX_ANSWERS) {
            ParsedAnswer *a = &answers[stored];
            if (type == 1 && rdlen == 4) {
                strcpy(a->owner, owner);
                memcpy(&a->address, packet + rdata, 4);
                a->ttl = ttl;
                a->type = type;
                ++stored;
            } else if (type == 5) {
                size_t name_cursor = rdata;
                strcpy(a->owner, owner);
                if (decode_name(packet, size, &name_cursor,
                                a->target, sizeof(a->target)) < 0 ||
                    name_cursor != rdata + rdlen)
                    return DNS_WIRE_INVALID;
                a->ttl = ttl;
                a->type = type;
                ++stored;
            }
        }
        cursor = rdata + rdlen;
    }
    if (cursor != size) return DNS_WIRE_INVALID;
    if (strlen(hostname) >= sizeof(current)) return DNS_WIRE_INVALID;
    strcpy(current, hostname);
    if (current[strlen(current) - 1] == '.')
        current[strlen(current) - 1] = '\0';
    for (depth = 0; depth < DNS_MAX_CNAME_DEPTH; ++depth) {
        int cname_index = -1;
        int address_index = -1;
        strcpy(seen[depth], current);
        for (i = 0; i < stored; ++i) {
            if (!name_equal(answers[i].owner, current)) continue;
            if (answers[i].type == 1 && address_index < 0)
                address_index = (int)i;
            if (answers[i].type == 5) {
                if (cname_index >= 0 &&
                    !name_equal(answers[cname_index].target,
                                answers[i].target))
                    return DNS_WIRE_INVALID;
                if (cname_index < 0) cname_index = (int)i;
            }
        }
        if (address_index >= 0 && cname_index >= 0) return DNS_WIRE_INVALID;
        if (address_index >= 0) {
            uint32_t ttl = answers[address_index].ttl;
            if (chain_ttl < ttl) ttl = chain_ttl;
            out->ipv4_be = answers[address_index].address;
            out->ttl = ttl;
            strcpy(out->canonical_name, current);
            return DNS_WIRE_OK;
        }
        if (cname_index < 0) {
            if (depth > 0) {
                strcpy(out->canonical_name, current);
                out->ttl = chain_ttl;
                return DNS_WIRE_CNAME_ONLY;
            }
            return DNS_WIRE_RETRYABLE;
        }
        if (answers[cname_index].ttl < chain_ttl)
            chain_ttl = answers[cname_index].ttl;
        for (i = 0; i <= (unsigned int)depth; ++i)
            if (name_equal(seen[i], answers[cname_index].target))
                return DNS_WIRE_INVALID;
        strcpy(current, answers[cname_index].target);
    }
    return DNS_WIRE_INVALID;
}
