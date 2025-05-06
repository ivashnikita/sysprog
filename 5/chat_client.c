#include "chat.h"
#include "chat_client.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <poll.h>
#include <stdio.h>
#include <errno.h>

#define BUFFER_SIZE 4096

struct chat_client {
    int socket;
    struct rlist messages;
    char *recv_buf;
    size_t recv_len;
    const char *name;
};

static void
make_fd_nonblocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFD, old_flags | O_NONBLOCK);
}

static void
parse_addr(const char *addr, char **host, char **port) {
    char *colon = strrchr(addr, ':');
    *host = strndup(addr, colon - addr);
    *port = strdup(colon + 1);
}

struct chat_client *
chat_client_new(const char *name) {
    struct chat_client *client = calloc(1, sizeof(*client));

    client->name = name;
    client->socket = -1;
    rlist_create(&client->messages);

    return client;
}

void
chat_client_delete(struct chat_client *client) {
    if (client->socket >= 0) {
        close(client->socket);
    }

    free(client->recv_buf);

    struct chat_message *msg;
    while ((msg = chat_client_pop_next(client))) {
        if (msg->data) {
            free(msg->data);
        }

        free(msg);
    }

    free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr) {
    if (client->socket != -1) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    char *host, *port;
    parse_addr(addr, &host, &port);
    if (!host || !port) {
        free(host);
        free(port);
        return CHAT_ERR_NO_ADDR;
    }

    struct addrinfo *res;
    struct addrinfo filter;
    memset(&filter, 0, sizeof(filter));
    filter.ai_family = AF_INET;
    filter.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &filter, &res);
    free(host);
    free(port);
    if (rc != 0) {
        return CHAT_ERR_NO_ADDR;
    }

    int client_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (client_fd == -1) {
        freeaddrinfo(res);
        return CHAT_ERR_SYS;
    }

    make_fd_nonblocking(client_fd);

    rc = connect(client_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) {
        close(client_fd);
        return CHAT_ERR_SYS;
    }

    client->socket = client_fd;

    return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client) {
    if (rlist_empty(&client->messages)) {
        return NULL;
    }

    struct rlist *item = rlist_shift(&client->messages);

    return rlist_entry(item, struct chat_message, node);
}

int
chat_client_update(struct chat_client *client, double timeout) {
    if (client->socket == -1) {
        return CHAT_ERR_NOT_STARTED;
    }

    struct pollfd fds;
    fds.fd = client->socket;
    fds.events = POLLIN;

    int nfds = poll(&fds, 1, (int)(timeout * 1000));
    if (nfds == -1) {
        return CHAT_ERR_SYS;
    }
    if (nfds == 0) {
        return CHAT_ERR_TIMEOUT;
    }

    if (fds.revents & POLLIN) {
        char buf[BUFFER_SIZE];
        ssize_t rc = read(client->socket, buf, sizeof(buf));

        if (rc != -1) {
            char *new_buf = realloc(client->recv_buf, client->recv_len + rc);
            client->recv_buf = new_buf;
            memcpy(client->recv_buf + client->recv_len, buf, rc);
            client->recv_len += rc;

            char *start = client->recv_buf;
            char *end;
            while ((end = memchr(start, '\n', client->recv_buf + client->recv_len - start))) {
                size_t len = end - start;
                struct chat_message *msg = calloc(1, sizeof(*msg));

                msg->data = malloc(len + 1);
                memcpy(msg->data, start, len);
                msg->data[len] = '\0';

                rlist_add_tail(&client->messages, &msg->node);
                start = end + 1;
            }

            size_t remaining = client->recv_buf + client->recv_len - start;
            if (remaining > 0 && start != client->recv_buf) {
                memmove(client->recv_buf, start, remaining);
            }

            client->recv_len = remaining;
        } else {
            close(client->socket);
            client->socket = -1;
            return CHAT_ERR_SYS;
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
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size) {
    ssize_t sent = send(client->socket, msg, msg_size, 0);
    if (sent == -1) {
        return CHAT_ERR_SYS;
    }

    return 0;
}