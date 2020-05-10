#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

bool tcp_nodelay(int fd) {
  int on = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    perror("setsockopt for tcp_nodelay");
    return false;
  }
  return true;
}

bool so_reuseaddr(int fd) {
  int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt for so_reuseaddr");
    return false;
  }
  return true;
}

bool nonblocking(int fd) {
  // set non blocking
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    perror("fcntl when setting non blocking");
    return false;
  }
  flags |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) < 0) {
    perror("fcntl when setting non blocking");
    return false;
  }
  return true;
}