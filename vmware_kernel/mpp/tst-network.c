#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include "network.h"

#define IP    "127.0.0.1"
#define SPORT 45678
#define CPORT (SPORT + 1)

void *server_thread(void *unused)
{
	int           rc;
	sock_handle_t h;
	int           n;
	ssize_t       bc;
	char          buf[4096];
	uint64_t      c;

	rc = server_setup(IP, strlen(IP), SPORT, &h);
	if (rc < 0) {
		printf("%s server_setup: FAILED\n", __func__);
		return NULL;
	}
	printf("%s server_setup: DONE\n", __func__);

	n = socket_accept(h, NULL, NULL);
	if (n < 0) {
		printf("%s socket_accept: FAILED\n", __func__);
		goto out;
	}
	printf("%s socket_accept: DONE\n", __func__);

	c = 0;
	while (1) {
		bc = socket_read(n, buf, sizeof(buf));
		if (bc != sizeof(buf)) {
			fprintf(stderr, "%s socket_read(%"PRIu64"): FAILED\n",
					__func__, c);
			break;
		}

		c++;
	}

	socket_close(n);
	printf("%s socket_close(client): DONE\n", __func__);

out:
	socket_close(h);
	printf("%s socket_close(server): DONE\n", __func__);
	return NULL;
}

void *client_thread(void *unused)
{
	int           rc;
	sock_handle_t h;
	uint64_t      i;
	ssize_t       bc;
	char          buf[4096];

	rc = client_setup(IP, strlen(IP), SPORT, IP, strlen(IP), CPORT, &h);
	if (rc < 0) {
		printf("%s client_setup: FAILED\n", __func__);
		return NULL;
	}
	printf("%s client_setup: DONE\n", __func__);

	for (i = 0; i < 1000000; i++) {
		bc = socket_write(h, buf, sizeof(buf));
		if (bc != sizeof(buf)) {
			fprintf(stderr, "%s socket_write(%"PRIu64"): FAILED\n",
					__func__, i);
			break;
		}
	}

	socket_close(h);
	printf("%s socket_close(client): DONE\n", __func__);
	return NULL;
}

int main(void)
{
	pthread_t sh;
	pthread_t ch;
	int       rc;

	rc = pthread_create(&sh, NULL, server_thread, NULL);
	if (rc < 0) {
		exit(1);
	}

	rc = pthread_create(&ch, NULL, client_thread, NULL);
	if (rc < 0) {
		exit(1);
	}

	rc = pthread_join(sh, NULL);
	assert(rc == 0);

	pthread_join(ch, NULL);
	assert(rc == 0);

	return 0;
}
