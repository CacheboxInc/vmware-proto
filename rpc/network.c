#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "network.h"

#ifdef VMWARE
typedef struct vmware_socket {
	vmk_Socket      sock;
	int             enabled;
	sock_handle_t   handle;
} vmware_socket_t;

vmware_socket_t *vmw_socks;
#endif

#ifdef VMWARE
int vmware_socket_sys_init(void)
{
	vmw_socks = calloc(MAX_SOCKETS, sizeof(*vmw_socks));
	if (vmw_socks == NULL) {
		return -1;
	}
	return 0;
}

static inline vmware_socket_t *vmware_socket_find_free(void)
{
	int             i;
	vmware_socket_t *vs;

	for (i = 0; i < MAX_SOCKETS; i++) {
		vs = &vmw_socks[i];
		if (vs->enabled == 0) {
			vs->handle  = i;
			vs->enabled = 0;
			return vs;
		}
	}
	return NULL;
}

static inline void vmware_socket_free(vmware_socket_t *vs)
{
	memset(&vs->sock, 0, sizeof(vs->sock));
	vs->enabled = 0;
}

static inline vmware_socket_t *vmware_socket_get(sock_handle_t h)
{
	if (h < 0 || h > MAX_SOCKETS) {
		return NULL;
	}

	return &vmw_socks[h];
}
#endif

#ifdef VMWARE
static sock_handle_t socket(int domain, int type, int protocol)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;

	vs = find_free_socket();
	if (vs == NULL) {
		return -1;
	}
	assert(vs->handle > 0 && vs->handle < MAX_SOCKETS);

	rc = vmk_SocketCreate(domain, type, protocol, &vs->sock);
	if (rc != VMK_OK) {
		free_socket(&vs);
		return -1;
	}

	vs->enabled = 1;
	return vs->handle;
}
#endif

int socket_create(int domain, int type, int protocol)
{
	return socket(domain, type, protocol);
}

#ifdef VMWARE
int bind(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;

	if (handle < 0 || handle > MAX_SOCKETS) {
		return -1;
	}

	vs = vmware_socket_get(handle);
	if (vs == NULL) {
		return -1;
	}

	rc = vmk_SocketBind(vs->sock, addr, len);
	if (rc != VMK_OK) {
		return -1;
	}

	return 0;
}
#endif

static inline int sockaddr_fill(sockaddr_in_t *addr, sa_family_t family,
		in_port_t port, char *ip, size_t size)
{
#ifdef VMWARE
	VMK_ReturnStatus rc;

	memset(addr, 0, sizeof(*addr));

	rc = vmk_SocketStringToAddr(family, ip, size, (sockaddr_t *) addr);
	if (rc != VMK_OK) {
		return -1;
	}

	addr->sin_len = sizeof(*addr);
	addr->sin_family = family;
	addri->sin_port = vmk_Htons(port);
#else
	memset(addr, 0, sizeof(*addr));

	addr->sin_family = family;
	addr->sin_port   = port;
	if (inet_aton(ip, &addr->sin_addr) == 0) {
		return -1;
	}
#endif
	return 0;
}

int socket_bind(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	return bind(handle, addr, len);
}

#ifndef VMWARE
int socket_listen(sock_handle_t handle, int backlog)
{
	return listen(handle, backlog);
}

int socket_accept(sock_handle_t handle, sockaddr_t *addr, socklen_t *len)
{
	return accept(handle, addr, len);
}
#endif

#ifdef VMWARE
int connect(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;

	if (handle < 0 || handle > MAX_SOCKETS) {
		return -1;
	}

	vs = vmware_socket_get(handle);
	if (vs == NULL) {
		return -1;
	}

	rc = vmk_SocketConnect(vs->sock, addr, len);
	if (rc != VMK_OK) {
		return -1;
	}

	return 0;
}
#endif

int socket_connect(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	return connect(handle, addr, len);
}

#ifdef VMWARE
static ssize_t read(sock_handle_t handle, char *buf, size_t count)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;
	int              br;

	if (handle < 0 || handle > MAX_SOCKETS) {
		return -1;
	}

	vs = vmware_socket_get(handle);
	if (vs == NULL) {
		return -1;
	}

	rc = vmk_SocketRecvFrom(&vs->sock, MSG_WAITALL, NULL, NULL, buf, count,
			&br);
	if (rc != VMK_OK) {
		return -1;
	}

	return br;
}
#endif

static ssize_t safe_read(sock_handle_t handle, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = read(handle, b, count);
		if (rc < 0 || rc == 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
	}

	return bc;
}

ssize_t socket_read(sock_handle_t handle, char *buf, size_t count)
{
	return safe_read(handle, buf, count);
}

#ifdef VMWARE
static ssize_t write(sock_handle_t handle, char *buf, size_t count)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;
	int              bw;

	if (handle < 0 || handle > MAX_SOCKETS) {
		return -1;
	}

	vs = vmware_socket_get(handle);
	if (vs == NULL) {
		return -1;
	}

	rc = vmk_SocketSendTo(&vs->sock, 0, NULL, buf, count, &bw);
	if (rc != VMK_OK) {
		return -1;
	}

	return bw;
}
#endif

static ssize_t safe_write(sock_handle_t handle, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = write(handle, b, count);
		if (rc < 0 || rc == 0) {
			if (errno == EINTR) {
				continue;
			}

			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
	}

	return bc;
}

ssize_t socket_write(sock_handle_t handle, char *buf, size_t count)
{
	return safe_write(handle, buf, count);
}

#ifdef VMWARE
int close(sock_handle_t handle)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;

	if (handle < 0 || handle > MAX_SOCKETS) {
		return -1;
	}

	vs = vmware_socket_get(handle);
	if (vs == NULL) {
		return -1;
	}

	rc = vmk_SocketShutdown(&vs->sock, SHUT_RDWR);
	if (rc != VMK_OK) {
		/* ignore error */
	}

	rc = vmk_SocketClose(&vs->sock);
	if (rc != VMK_OK) {
		return -1;
	}

	vmware_socket_free(vs);
	return 0;
}
#endif

int socket_close(sock_handle_t handle)
{
	return close(handle);
}

#ifndef VMWARE
int server_setup(char *ip, int ip_len, int port, sock_handle_t *handle)
{
	sockaddr_in_t a;
	sock_handle_t sh;
	int           rc;

	*handle = -1;
	sh      = socket_create(PF_INET, SOCK_STREAM, 0);
	if (sh < 0) {
		return -1;
	}

	rc = sockaddr_fill(&a, AF_INET, port, ip, ip_len);
	if (rc < 0) {
		goto error;
	}

	rc = socket_bind(sh, (sockaddr_t *) &a, sizeof(a));
	if (rc < 0) {
		goto error;
	}

	rc = socket_listen(sh, MAX_BACKLOG);
	if (rc < 0) {
		goto error;
	}

	*handle = sh;
	return 0;
error:
	socket_close(sh);
	return -1;
}
#endif

int client_setup(char *dip, int dip_len, int dport, char *sip, int sip_len,
		int sport, sock_handle_t *handle)
{
	sock_handle_t sh;
	int           rc;
	sockaddr_in_t sa;
	sockaddr_in_t da; /* destination IP address representation */

	*handle = -1;
	sh      = socket_create(PF_INET, SOCK_STREAM, 0);
	if (sh < 0) {
		return -1;
	}

	rc = sockaddr_fill(&sa, AF_INET, sport, sip, sip_len);
	if (rc < 0) {
		goto error;
	}

	rc = socket_bind(sh, (sockaddr_t *) &sa, sizeof(sa));
	if (rc < 0) {
		goto error;
	}

	rc = sockaddr_fill(&da, AF_INET, dport, dip, dip_len);
	if (rc < 0) {
		goto error;
	}

	rc = socket_connect(sh, (sockaddr_t *) &da, sizeof(sa));
	if (rc < 0) {
		goto error;
	}

	*handle = sh;
	return 0;

error:
	socket_close(sh);
	return -1;
}
