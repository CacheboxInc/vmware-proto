#ifndef __NETWORK_H__
#define __NETWORK_H__

// #define VMWARE
#include <stddef.h>

static const int MAX_SOCKETS = 16;
static const int MAX_BACKLOG = 16;
typedef int sock_handle_t;
typedef vmk_int32 ssize_t;

typedef struct   vmk_SocketAddress   sockaddr_t;
typedef struct   vmk_SocketIPAddress sockaddr_in_t;
typedef vmk_uint8                      sa_family_t;
typedef vmk_uint16                     in_port_t;
typedef int                          socklen_t;

int     socket_create(int domain, int type, int protocol);
int     socket_bind(sock_handle_t, sockaddr_t *addr, socklen_t len);
int     socket_connect(sock_handle_t, sockaddr_t *addr, socklen_t len);
ssize_t socket_read(sock_handle_t, char *buf, size_t count);
ssize_t socket_write(sock_handle_t, char *buf, size_t count);
int     socket_close(sock_handle_t handle);
int vmware_socket_sys_init(char *name, module_global_t *module);
int vmware_socket_sys_deinit(void);

int client_setup(char *dip, int dip_len, int dpor, char *sip, int sip_len,
		int sport, sock_handle_t *handle);
#endif
