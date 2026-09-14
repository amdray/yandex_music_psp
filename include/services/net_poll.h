#ifndef YM_SERVICES_NET_POLL_H
#define YM_SERVICES_NET_POLL_H

#include <stddef.h>
#include <pspnet_inet.h>

/* Compatibility declarations for PSPDEV installations predating the public
 * sceNetInetPoll declaration. libpspnet_inet already exports the function. */
#ifndef SCE_NET_INET_POLLIN
#define SCE_NET_INET_POLLIN   0x0001
#define SCE_NET_INET_POLLOUT  0x0004
#define SCE_NET_INET_POLLERR  0x0008
#define SCE_NET_INET_POLLHUP  0x0010
#define SCE_NET_INET_POLLNVAL 0x0020

struct SceNetInetPollfd {
    int   fd;
    short events;
    short revents;
};

int sceNetInetPoll(struct SceNetInetPollfd *fds, size_t nfds, int timeout);
#endif

#endif /* YM_SERVICES_NET_POLL_H */
