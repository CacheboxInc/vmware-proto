#include "vmkapi.h"
#include "vmkapi_socket.h"
#include "vmkapi_socket_ip.h"
#include "vmware_include.h"
#include "network.h"

static void print_error(VMK_ReturnStatus s)
{
	switch (s) {
	case VMK_OK:
		vmk_WarningMessage("VMK_OK\n");
		break;
	case VMK_BAD_PARAM:
		vmk_WarningMessage("VMK_BAD_PARAM\n");
		break;
	case VMK_NO_MEMORY:
		vmk_WarningMessage("VMK_NO_MEMORY\n");
		break;
	case VMK_EPROTONOSUPPORT:
		vmk_WarningMessage("VMK_EPROTONOSUPPORT\n");
		break;
	case VMK_BAD_PARAM_TYPE:
		vmk_WarningMessage("VMK_BAD_PARAM_TYPE\n");
		break;
	case VMK_NO_BUFFERSPACE:
		vmk_WarningMessage("VMK_NO_BUFFERSPACE\n");
		break;
	case VMK_EOPNOTSUPP:
		vmk_WarningMessage("VMK_EOPNOTSUPP\n");
		break;
	case VMK_NO_ACCESS:
		vmk_WarningMessage("VMK_NO_ACCESS\n");
		break;
	default:
		vmk_WarningMessage("default\n");
		break;
	}
}

int socket_create(vmk_Socket *sock, int domain, int type, int protocol)
{
	VMK_ReturnStatus rc;

	rc = vmk_SocketCreate(domain, type, protocol, sock);
	if (rc != VMK_OK) {
		vmk_WarningMessage("%s: vmk_SocketCreate failed\n", __func__);
		print_error(rc);
		return -1;
	}

	vmk_WarningMessage("%s: socket created.\n", __func__);
	return 0;
}

int socket_bind(vmk_Socket sock, sockaddr_t *addr, socklen_t len)
{
	VMK_ReturnStatus rc;

	rc = vmk_SocketBind(sock, addr, len);
	if (rc != VMK_OK) {
		vmk_WarningMessage("%s: vmk_SocketBind failed\n", __func__);
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

int socket_connect(vmk_Socket sock, sockaddr_t *addr, socklen_t len)
{
	VMK_ReturnStatus rc;

	rc = vmk_SocketConnect(sock, addr, len);
	if (rc != VMK_OK) {
		vmk_WarningMessage("%s: vmk_SocketConnect failed\n", __func__);
		return -1;
	}

	return 0;
}

static ssize_t read(vmk_Socket sock, char *buf, size_t count)
{
	VMK_ReturnStatus rc;
	int              br;

	rc = vmk_SocketRecvFrom(sock, 0, NULL, NULL, buf, count,
			&br);
	if (rc != VMK_OK) {
		return -1;
	}

	return br;
}

ssize_t socket_read(vmk_Socket sock, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = read(sock, b, count);
		if (rc < 0 || rc == 0) {
			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
	}

	return bc;
}

static ssize_t write(vmk_Socket sock, char *buf, size_t count)
{
	VMK_ReturnStatus rc;
	int              bw;

	rc = vmk_SocketSendTo(sock, 0, NULL, buf, count, &bw);
	if (rc != VMK_OK) {
		return -1;
	}

	return bw;
}

ssize_t socket_write(vmk_Socket sock, char *buf, size_t count)
{
	char    *b;
	ssize_t bc;
	ssize_t rc;

	b  = buf;
	bc = 0;
	while (count != 0) {
		rc = write(sock, b, count);
		if (rc < 0 || rc == 0) {
			break;
		}

		count -= rc;
		b     += rc;
		bc    += rc;
	}

	return bc;
}

int socket_close(vmk_Socket sock)
{
	VMK_ReturnStatus rc;

	rc = vmk_SocketShutdown(sock, VMK_SOCKET_SHUT_RDWR);
	if (rc != VMK_OK) {
		/* ignore error */
	}

	rc = vmk_SocketClose(sock);
	if (rc != VMK_OK) {
		return -1;
	}

	return 0;
}

int client_setup(char *dip, int dip_len, int dport, char *sip, int sip_len,
		int sport, vmk_Socket *sock)
{
	int           rc;
	sockaddr_in_t sa;
	sockaddr_in_t da; /* destination IP address representation */

	rc = socket_create(sock, VMK_SOCKET_AF_INET, VMK_SOCKET_SOCK_STREAM, 0);
	if (rc < 0) {
		vmk_WarningMessage("socket_create failed.\n");
		return -1;
	}

	rc = sockaddr_fill(&sa, VMK_SOCKET_AF_INET, sport, sip, sip_len);
	if (rc < 0) {
		vmk_WarningMessage("1 sockaddr_fill failed.\n");
		goto error;
	}

	rc = socket_bind(*sock, (sockaddr_t *) &sa, sizeof(sa));
	if (rc < 0) {
		vmk_WarningMessage("socket_bind failed.\n");
		goto error;
	}

	rc = sockaddr_fill(&da, VMK_SOCKET_AF_INET, dport, dip, dip_len);
	if (rc < 0) {
		vmk_WarningMessage("2 sockaddr_fill failed.\n");
		goto error;
	}

	rc = socket_connect(*sock, (sockaddr_t *) &da, sizeof(sa));
	if (rc < 0) {
		vmk_WarningMessage("socket_connect failed.\n");
		goto error;
	}

	vmk_WarningMessage("%s <==\n", __func__);
	return 0;

error:
	socket_close(*sock);
	return -1;
}
