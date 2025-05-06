#include "chat.h"
#include "chat_server.h"
#include <sys/epoll.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_CONN 1000
#define MAX_EVENTS 1000
#define BUFFER_SIZE 1024

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	/** Output buffer. */
	char buffer[BUFFER_SIZE];
	struct chat_peer *next;
	struct chat_peer *prev;
};

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket;
	/** Array of peers. */
	struct chat_peer *peers;
	int epoll_fd;
};

static void
make_fd_nonblocking(int fd) {
	int old_flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFD, old_flags | O_NONBLOCK);
}

int
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
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));
	server->peers = NULL;
	server->socket = -1;

	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
		close(server->socket);

	/* IMPLEMENT THIS FUNCTION */

	free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket != -1) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		return CHAT_ERR_SYS;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		return CHAT_ERR_PORT_BUSY;
	}

	if (listen(server_fd, MAX_CONN) == -1) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		close(server_fd);
		return CHAT_ERR_SYS;
	}

	if (epoll_ctl_add(epoll_fd, server_fd, NULL, EPOLLIN | EPOLLET) != 0) {
		close(server_fd);
		return CHAT_ERR_SYS;
	}

	server->socket = server_fd;
	server->epoll_fd = epoll_fd;

	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)server;
	return NULL;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	/*
	 * 1) Wait on epoll/kqueue/poll for update on any socket.
	 * 2) Handle the update.
	 * 2.1) If the update was on listen-socket, then you probably need to
	 *     call accept() on it - a new client wants to join.
	 * 2.2) If the update was on a client-socket, then you might want to
	 *     read/write on it.
	 */
	if (server->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	struct epoll_event new_events[MAX_EVENTS];
	int nfds = epoll_wait(server->epoll_fd, new_events, MAX_EVENTS, timeout);
	if (nfds == 0) {
		return CHAT_ERR_TIMEOUT;
	}
	if (nfds == -1) {
		return CHAT_ERR_SYS;
	}

	for (int i = 0; i < nfds; i++) {
		if (new_events[i].data.ptr == NULL) {
			int peer_fd = accept(server->socket, NULL, NULL);
			if (peer_fd == -1) {
				return CHAT_ERR_SYS;
			}

			struct chat_peer *p = malloc(sizeof(*p));
			p->socket = peer_fd;
			p->next = server->peers;
			p->prev = NULL;
			if (server->peers != NULL) {
				server->peers->prev = p;
			}
			server->peers = p;

			if (epoll_ctl_add(server->epoll_fd, peer_fd, p, EPOLLIN | EPOLLET) != 0) {
				free(p);
				return CHAT_ERR_SYS;
			}
		} else if (new_events[i].events & EPOLLIN) {

		}
	}

	return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#endif
	(void)server;
	return -1;
}

int
chat_server_get_socket(const struct chat_server *server)
{
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
