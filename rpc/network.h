#ifndef __NETWORK_H__
#define __NETWORK_H__

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// #define VMWARE

static const int MAX_SOCKETS = 16;
static const int MAX_BACKLOG = 16;
typedef int sock_handle_t;

#ifdef VMWARE
typedef struct   vmk_SocketAddress   sockaddr_t;
typedef struct   vmk_SocketIPAddress sockaddr_in_t;
typedef uint8_t                      sa_family_t;
typedef uint16_t                     in_port_t;
typedef int                          socklen_t;
#else
typedef struct   sockaddr            sockaddr_t;
typedef struct   sockaddr_in         sockaddr_in_t;
#endif

int     socket_create(int domain, int type, int protocol);
int     socket_bind(sock_handle_t, sockaddr_t *addr, socklen_t len);
int     socket_connect(sock_handle_t, sockaddr_t *addr, socklen_t len);
ssize_t socket_read(sock_handle_t, char *buf, size_t count);
ssize_t socket_write(sock_handle_t, char *buf, size_t count);
int     socket_close(sock_handle_t handle);

#ifndef VMWARE
int socket_accept(int handle, sockaddr_t *addr, socklen_t *len);
int socket_listen(int handle, int backlog);
int server_setup(char *ip, int ip_len, int port, sock_handle_t *handle);
#else
int vmware_socket_sys_init(void);
#endif

int client_setup(char *dip, int dip_len, int dpor, char *sip, int sip_len,
		int sport, sock_handle_t *handle);
#endif
