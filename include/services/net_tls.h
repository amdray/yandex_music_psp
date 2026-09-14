#ifndef YM_SERVICES_NET_TLS_H
#define YM_SERVICES_NET_TLS_H

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>

// Конфигурация TLS
typedef struct {
    int verify_mode;                // MBEDTLS_SSL_VERIFY_NONE или MBEDTLS_SSL_VERIFY_REQUIRED
    int debug_threshold;             // Порог отладки mbedTLS (0 = отключено)
    int enable_verbose_logging;      // Включить детальное логирование в hot path (0/1)
} NetTlsConfig;

// Контекст TLS соединения
// net должен оставаться первым полем: bio-колбэки получают &conn->net и
// кастуют его обратно к NetTlsConnection.
typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    int socket_fd;
    int network_lease_held;
    unsigned int network_generation;
    // Дедлайн «нет данных N с» для неблокирующего recv/send
    // (us, sceKernelGetSystemTimeWide). 0 = не взведён; сброс при прогрессе.
    unsigned long long recv_deadline_us;
    unsigned long long send_deadline_us;
} NetTlsConnection;

/* Stable transport categories propagated through HTTP.  mbedTLS native error
 * values remain reserved for read/write operations. */
#define NET_TLS_ERR_INVALID    (-20001)
#define NET_TLS_ERR_OFFLINE    (-20002)
#define NET_TLS_ERR_DNS        (-20003)
#define NET_TLS_ERR_CONNECT    (-20004)
#define NET_TLS_ERR_HANDSHAKE  (-20005)
#define NET_TLS_ERR_CANCELLED  (-20006)

// Инициализация TLS конфигурации
// config может быть NULL для значений по умолчанию
int net_tls_config_init(const NetTlsConfig *config);
int net_tls_config_shutdown(void);
int net_tls_config_is_ready(void);

// Установка TLS соединения
// hostname: имя хоста для SNI
// port: порт (обычно 443)
// out_conn: выходной контекст соединения
int net_tls_connect(const char *hostname, int port, NetTlsConnection *out_conn);
void net_tls_disconnect(NetTlsConnection *conn);
int net_tls_connection_is_current(const NetTlsConnection *conn);

// Чтение/запись через TLS
int net_tls_read(NetTlsConnection *conn, unsigned char *buf, size_t len);
int net_tls_write(NetTlsConnection *conn, const unsigned char *buf, size_t len);

// Кооперативная отмена TLS-операций для вызывающего потока.
// bind привязывает флаг к текущему потоку: пока *flag != 0, все TLS-ожидания
// этого потока (recv/send EAGAIN-циклы, handshake, ожидание handshake-семафора)
// завершаются ошибкой в пределах сотен миллисекунд вместо полного таймаута.
// unbind обязателен в том же потоке по окончании работы (иначе слот таблицы
// останется занят чужим tid). Потоки без bind ведут себя как раньше.
void net_tls_cancel_bind(volatile int *flag);
void net_tls_cancel_unbind(void);

/* Recovery and shutdown close admission and wait for connection owners to
 * release their TLS contexts before APCTL or shared TLS state is changed. */
void net_tls_quiesce_begin(void);
int net_tls_quiesce_wait(unsigned int timeout_us);
void net_tls_quiesce_end(void);

#endif
