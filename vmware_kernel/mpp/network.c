#include "vmkapi.h"
#include "vmkapi_socket.h"
#include "vmkapi_socket_ip.h"
#include "vmware_include.h"
#include "network.h"

typedef struct vmware_socket {
	vmk_Socket      sock;
	int             enabled;
	sock_handle_t   handle;
} vmware_socket_t;

static vmk_HeapID vmw_socks_heap_id;
vmware_socket_t   *vmw_socks;

int vmware_socket_sys_init(char *name, module_global_t *module)
{
	int rc;

	rc = vmware_heap_create(&vmw_socks_heap_id, name, module, MAX_SOCKETS,
			sizeof(*vmw_socks));
	if (rc < 0) {
		return -1;
	}

	vmw_socks = calloc(vmw_socks_heap_id, MAX_SOCKETS, sizeof(*vmw_socks));
	if (vmw_socks == NULL) {
		vmk_WarningMessage("Failed to allocate memory for vmw_socks");
		vmk_HeapDestroy(vmw_socks_heap_id);
		return -1;
	}

	return 0;
}

int vmware_socket_sys_deinit(void)
{
	if (vmw_socks) {
		free(vmw_socks_heap_id, vmw_socks);
	}

	vmk_HeapDestroy(vmw_socks_heap_id);
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

static sock_handle_t socket(int domain, int type, int protocol)
{
	vmware_socket_t  *vs;
	VMK_ReturnStatus rc;

	vs = vmware_socket_find_free();
	if (vs == NULL) {
		return -1;
	}
	assert(vs->handle > 0 && vs->handle < MAX_SOCKETS);

	rc = vmk_SocketCreate(domain, type, protocol, &vs->sock);
	if (rc != VMK_OK) {
		vmware_socket_free(vs);
		return -1;
	}

	vs->enabled = 1;
	return vs->handle;
}

int socket_create(int domain, int type, int protocol)
{
	return socket(domain, type, protocol);
}

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

static inline int sockaddr_fill(sockaddr_in_t *addr, sa_family_t family,
		in_port_t port, char *ip, size_t size)
{
	VMK_ReturnStatus rc;

	memset(addr, 0, sizeof(*addr));

	rc = vmk_SocketStringToAddr(family, ip, size, (sockaddr_t *) addr);
	if (rc != VMK_OK) {
		return -1;
	}

	addr->sin_len = sizeof(*addr);
	addr->sin_family = family;
	addr->sin_port = vmk_Htons(port);
	return 0;
}

int socket_bind(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	return bind(handle, addr, len);
}


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

int socket_connect(sock_handle_t handle, sockaddr_t *addr, socklen_t len)
{
	return connect(handle, addr, len);
}

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

	rc = vmk_SocketRecvFrom(vs->sock, 0, NULL, NULL, buf, count,
			&br);
	if (rc != VMK_OK) {
		return -1;
	}

	return br;
}

static ssize_t safe_read(sock_handle_t handle, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;
	int 	errno;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = read(handle, b, count);
		if (rc < 0 || rc == 0) {
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

	rc = vmk_SocketSendTo(vs->sock, 0, NULL, buf, count, &bw);
	if (rc != VMK_OK) {
		return -1;
	}

	return bw;
}

static ssize_t safe_write(sock_handle_t handle, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;
	int 	errno;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = write(handle, b, count);
		if (rc < 0 || rc == 0) {
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

	rc = vmk_SocketShutdown(vs->sock, VMK_SOCKET_SHUT_RDWR);
	if (rc != VMK_OK) {
		/* ignore error */
	}

	rc = vmk_SocketClose(vs->sock);
	if (rc != VMK_OK) {
		return -1;
	}

	vmware_socket_free(vs);
	return 0;
}

int socket_close(sock_handle_t handle)
{
	return close(handle);
}

int client_setup(char *dip, int dip_len, int dport, char *sip, int sip_len,
		int sport, sock_handle_t *handle)
{
	sock_handle_t sh;
	int           rc;
	sockaddr_in_t sa;
	sockaddr_in_t da; /* destination IP address representation */

	*handle = -1;
	sh      = socket_create(VMK_SOCKET_AF_INET, VMK_SOCKET_SOCK_STREAM, 0);
	if (sh < 0) {
		return -1;
	}

	rc = sockaddr_fill(&sa, VMK_SOCKET_AF_INET, sport, sip, sip_len);
	if (rc < 0) {
		goto error;
	}

	rc = socket_bind(sh, (sockaddr_t *) &sa, sizeof(sa));
	if (rc < 0) {
		goto error;
	}

	rc = sockaddr_fill(&da, VMK_SOCKET_AF_INET, dport, dip, dip_len);
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
