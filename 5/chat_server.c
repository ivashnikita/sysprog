#include "chat.h"
#include "chat_server.h"
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_CONN 1000
#define MAX_EVENTS 1000
#define BUFFER_SIZE 4096

struct chat_peer {
    int socket;
    struct rlist node;
    char *recv_buf;
    size_t recv_len;
};

struct chat_server {
    int socket;
    struct rlist peers;
    int epoll_fd;
    struct rlist messages;
};

static void
make_fd_nonblocking(int fd) {
    int old_flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
}

static int
epoll_ctl_add(int epoll_fd, int fd, struct chat_peer *p, uint32_t events) {
    make_fd_nonblocking(fd);

    struct epoll_event new_event;
    new_event.data.ptr = p;
    new_event.events = events;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &new_event) == -1) {
        close(fd);
        return CHAT_ERR_SYS;
    }

    return 0;
}

struct chat_server *
chat_server_new(void) {
    struct chat_server *server = calloc(1, sizeof(*server));

    rlist_create(&server->peers);
    server->socket = -1;
    rlist_create(&server->messages);

    return server;
}

void
chat_server_delete(struct chat_server *server) {
    if (server->socket >= 0) {
        close(server->socket);
    }
    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
    }

    struct chat_peer *peer, *tmp;
    rlist_foreach_entry_safe(peer, &server->peers, node, tmp) {
        if (peer->socket >= 0) close(peer->socket);
        if (peer->recv_buf) free(peer->recv_buf);

        free(peer);
    }

    struct chat_message *msg;
    while ((msg = chat_server_pop_next(server))) {
        if (msg->data) {
            free(msg->data);
        }

        free(msg);
    }

    free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port) {
    if (server->socket != -1) {
        return CHAT_ERR_ALREADY_STARTED;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        return CHAT_ERR_SYS;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr))) {
        close(server_fd);
        return CHAT_ERR_PORT_BUSY;
    }

    if (listen(server_fd, MAX_CONN)) {
        close(server_fd);
        return CHAT_ERR_SYS;
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        close(server_fd);
        return CHAT_ERR_SYS;
    }

    if (epoll_ctl_add(epoll_fd, server_fd, NULL, EPOLLIN | EPOLLET)) {
        close(server_fd);
        close(epoll_fd);
        return CHAT_ERR_SYS;
    }

    server->socket = server_fd;
    server->epoll_fd = epoll_fd;

    return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server) {
    if (rlist_empty(&server->messages)) {
        return NULL;
    }

    struct rlist *item = rlist_shift(&server->messages);

    return rlist_entry(item, struct chat_message, node);
}

static void
broadcast_message(struct chat_server *server, struct chat_peer *sender, const char *data, size_t len) {
    struct chat_peer *peer;

    rlist_foreach_entry(peer, &server->peers, node) {
        if (peer != sender && peer->socket >= 0) {
            send(peer->socket, data, len, 0);
        }
    }
}

int
chat_server_update(struct chat_server *server, double timeout) {
    if (server->socket == -1) {
        return CHAT_ERR_NOT_STARTED;
    }

    struct epoll_event events[MAX_EVENTS];
    int nfds = epoll_wait(server->epoll_fd, events, MAX_EVENTS, (int)(timeout * 1000));
    if (nfds == -1) {
        return CHAT_ERR_SYS;
    }
    if (nfds == 0) {
        return CHAT_ERR_TIMEOUT;
    }

    for (int i = 0; i < nfds; i++) {
        if (events[i].data.ptr == NULL) {
            int peer_fd = accept(server->socket, NULL, NULL);
            if (peer_fd == -1) {
                continue;
            }

            struct chat_peer *peer = calloc(1, sizeof(*peer));
            peer->socket = peer_fd;
            rlist_add_tail(&server->peers, &peer->node);

            if (epoll_ctl_add(server->epoll_fd, peer_fd, peer, EPOLLIN | EPOLLET)) {
                rlist_del(&peer->node);
                close(peer_fd);
                free(peer);
            }
        } else if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
            struct chat_peer *peer = events[i].data.ptr;
            char buf[BUFFER_SIZE];
            ssize_t nread;
            bool peer_dead = false;

            while (1) {
                nread = read(peer->socket, buf, sizeof(buf));
                if (nread <= 0) {
                    if (nread == -1) {
                        break;
                    }

                    peer_dead = true;
                    break;
                }

                char *new_buf = realloc(peer->recv_buf, peer->recv_len + nread);

                peer->recv_buf = new_buf;
                memcpy(peer->recv_buf + peer->recv_len, buf, nread);
                peer->recv_len += nread;

                char *start = peer->recv_buf;
                char *end;
                while ((end = memchr(start, '\n', peer->recv_buf + peer->recv_len - start))) {
                    size_t len = end - start;
                    struct chat_message *msg = calloc(1, sizeof(*msg));
                    msg->data = malloc(len + 1);

                    memcpy(msg->data, start, len);
                    msg->data[len] = '\0';

                    rlist_add_tail(&server->messages, &msg->node);
                    broadcast_message(server, peer, start, end - start + 1);
                    start = end + 1;
                }

                size_t remaining = peer->recv_buf + peer->recv_len - start;

                if (remaining > 0 && start != peer->recv_buf) {
                    memmove(peer->recv_buf, start, remaining);
                }

                peer->recv_len = remaining;
            }

            if (peer_dead) {
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, peer->socket, NULL);
                close(peer->socket);
                rlist_del(&peer->node);
                free(peer->recv_buf);
                free(peer);
            }
        }
    }

    return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server) {
    return server->epoll_fd;
}

int
chat_server_get_socket(const struct chat_server *server) {
    return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
    if (server->socket >= 0) {
        return CHAT_EVENT_INPUT;
    }

    return 0;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}