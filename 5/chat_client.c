#include "chat.h"
#include "chat_client.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <poll.h>
#include <bits/fcntl-linux.h>

#include "../utils/rlist.h"

#define BUFFER_SIZE 1024

struct chat_client {
	/** Socket connected to the server. */
	int socket;
	/** Array of received messages. */
	struct rlist messages;
	char buffer[BUFFER_SIZE];
	/** Output buffer. */
	/* ... */
	const char *name;
};

static void
make_fd_nonblocking(int fd) {
	int old_flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFD, old_flags | O_NONBLOCK);
}

struct chat_client *
chat_client_new(const char *name)
{
	struct chat_client *client = calloc(1, sizeof(*client));
	client->name = name;
	client->socket = -1;
	rlist_create(&client->messages);

	return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);

	/* IMPLEMENT THIS FUNCTION */

	free(client);
}

void
parse_addr(const char *addr, char **host, char **port) {
	char *colon = strrchr(addr, ':');
	*host = strndup(addr, colon - addr);
	*port = strdup(colon + 1);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
	if (client->socket != -1) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	struct addrinfo *res;
	struct addrinfo filter;
	memset(&filter, 0, sizeof(filter));
	filter.ai_family = AF_INET;
	filter.ai_socktype = SOCK_STREAM;

	char *host, *port;
	parse_addr(addr, &host, &port);
	if (!host || !port) {
		free(host);
		free(port);
		return CHAT_ERR_NO_ADDR;
	}

	int rc = getaddrinfo(host, port, &filter, &res);
	free(host);
	free(port);
	if (rc != 0) {
		return CHAT_ERR_NO_ADDR;
	}
	if (res == NULL) {
		return CHAT_ERR_NO_ADDR;
	}

	int client_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	make_fd_nonblocking(client_fd);

	rc = connect(client_fd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (rc != 0) {
		close(client_fd);
		return CHAT_ERR_SYS;
	}

	client->socket = client_fd;
	send(client_fd, client->name, strlen(client->name), 0);

	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)client;
	return NULL;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	struct pollfd fds[1];
	fds[0].fd = client->socket;
	fds[0].events = POLLIN;

	int nfds = poll(fds, 1, timeout);

	if (nfds == -1) {
		return CHAT_ERR_SYS;
	} else if (nfds == 0) {
		return CHAT_ERR_TIMEOUT;
	}

	for (int i = 0; i < nfds; i++) {
		if (fds[i].fd == -1) {
			continue;
		}

		if (fds[i].events & POLLIN) {
			read(fds[i].fd, client->buffer, BUFFER_SIZE);
		} else if (fds[i].events & POLLOUT) {
			send(fds[i].fd, "hi", 3, 0);
			continue;
		}
	}

	return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (client->socket >= 0) {
		return CHAT_EVENT_INPUT;
	}

	return 0;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	send(client->socket, msg, msg_size, 0);
	return 0;
}
