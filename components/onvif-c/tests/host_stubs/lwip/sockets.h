/*
 * Host test stub: lwip/sockets.h — fully virtual UDP network.
 *
 * No real sockets are opened: socket()/bind()/recvfrom()/sendto()/close()
 * are renamed to fake_ implementations (see onvif_fake.c) operating on
 * injectable inbound datagrams and a captured outbound log. Only the call
 * surface onvif-c's discovery task uses is provided.
 */

#ifndef STUB_LWIP_SOCKETS_H
#define STUB_LWIP_SOCKETS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h> /* struct timeval (SO_RCVTIMEO payload; ignored) */

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
typedef uint32_t socklen_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr {
    unsigned short sa_family;
    char           sa_data[14];
};

struct sockaddr_in {
    unsigned short sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    char           sin_zero[8];
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

#define AF_INET           2
#define SOCK_DGRAM        2
#define SOL_SOCKET        1
#define SO_REUSEADDR      2
#define SO_RCVTIMEO       20
#define IPPROTO_IP        0
#define IP_ADD_MEMBERSHIP 35
#define IP_MULTICAST_IF   32
#define IP_MULTICAST_TTL  33
#define INADDR_ANY        0u

/* Host-tested platforms are little-endian; swap conditionally anyway. */
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static inline in_port_t htons(in_port_t v)
{
    return v;
}
static inline in_port_t ntohs(in_port_t v)
{
    return v;
}
static inline in_addr_t htonl(in_addr_t v)
{
    return v;
}
static inline in_addr_t ntohl(in_addr_t v)
{
    return v;
}
#else
static inline in_port_t htons(in_port_t v)
{
    return (in_port_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}
static inline in_port_t ntohs(in_port_t v)
{
    return htons(v);
}
static inline in_addr_t htonl(in_addr_t v)
{
    return (in_addr_t)(((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) |
                       ((v >> 24) & 0xFFu));
}
static inline in_addr_t ntohl(in_addr_t v)
{
    return htonl(v);
}
#endif

in_addr_t inet_addr(const char *cp);
char     *inet_ntoa(struct in_addr in);

/* Fake network (implemented in onvif_fake.c); macros rename the POSIX/lwIP
 * names so library code is compiled unmodified. */
int onvif_fake_socket(int domain, int type, int protocol);
int onvif_fake_bind(int fd, const struct sockaddr *addr, socklen_t len);
int onvif_fake_setsockopt(int fd, int level, int optname, const void *optval, socklen_t len);
int onvif_fake_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from,
                        socklen_t *fromlen);
int onvif_fake_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to,
                      socklen_t tolen);
int onvif_fake_close(int fd);

#define socket     onvif_fake_socket
#define bind       onvif_fake_bind
#define setsockopt onvif_fake_setsockopt
#define recvfrom   onvif_fake_recvfrom
#define sendto     onvif_fake_sendto
#define close      onvif_fake_close

#endif /* STUB_LWIP_SOCKETS_H */
