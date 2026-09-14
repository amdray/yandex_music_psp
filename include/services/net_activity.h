#ifndef YM_SERVICES_NET_ACTIVITY_H
#define YM_SERVICES_NET_ACTIVITY_H

typedef struct NetActivitySnapshot {
    unsigned int active_response_count;
    unsigned int body_activity_epoch;
} NetActivitySnapshot;

void net_activity_begin(void);
void net_activity_end(void);
void net_activity_body_bytes(int byte_count);
void net_activity_get_snapshot(NetActivitySnapshot *out);

#endif
