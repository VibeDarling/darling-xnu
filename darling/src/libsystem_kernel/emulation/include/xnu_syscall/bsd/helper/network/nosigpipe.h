#ifndef NETWORK_NOSIGPIPE_H
#define NETWORK_NOSIGPIPE_H

#include <stdbool.h>

/* Darwin lets a descriptor opt out of SIGPIPE (fcntl F_SETNOSIGPIPE, setsockopt SO_NOSIGPIPE):
 * a write to a closed peer then fails with EPIPE instead of raising the signal. Linux has no
 * such descriptor flag, only MSG_NOSIGNAL per send, so the flag is kept here and the send and
 * write paths add MSG_NOSIGNAL for a descriptor that has it. */
int nosigpipe_set(int fd, bool enabled);
bool nosigpipe_get(int fd);
void nosigpipe_closed(int fd);

#endif // NETWORK_NOSIGPIPE_H
