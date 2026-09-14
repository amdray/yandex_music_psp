#ifndef YM_SERVICES_COVER_HTTP_CLIENT_H
#define YM_SERVICES_COVER_HTTP_CLIENT_H

#include "services/net_tls.h"

typedef struct {
    NetTlsConnection conn;
    int connected;
} CoverHttpClient;

void cover_http_client_init(CoverHttpClient *client);
void cover_http_client_shutdown(CoverHttpClient *client);

// Download URL (must be https://avatars.yandex.net/...) to file_path.
// Returns 0 on success, -1 on error.
int cover_http_client_download(CoverHttpClient *client,
                               const char *url,
                               const char *file_path);

#endif
