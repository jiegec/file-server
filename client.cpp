#include <algorithm>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#define eprintf(...) fprintf(stderr, __VA_ARGS__)

int main(int argc, char *argv[]) {
  if (argc != 6) {
    eprintf("Usage: %s addr port download|upload local_path remote_path\n",
            argv[0]);
    return 1;
  }
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  int error = getaddrinfo(argv[1], argv[2], &hints, &res);
  if (error != 0) {
    eprintf("getaddrinfo: %s\n", gai_strerror(error));
    return 1;
  }

  bool found = false;
  for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
    // print info
    char hbuf[NI_MAXHOST];
    char sbuf[NI_MAXSERV];
    error = getnameinfo(p->ai_addr, p->ai_addrlen, hbuf, sizeof(hbuf), sbuf,
                        sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
    if (error) {
      eprintf("getaddrinfo: %s\n", gai_strerror(error));
      continue;
    }
    printf("connecting to %s:%s\n", hbuf, sbuf);

    // connect
    int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, p->ai_addr, p->ai_addrlen) < 0) {
      close(fd);
      continue;
    }

    printf("success!\n");
    found = true;
    // TODO
    close(fd);
    break;
  }
  freeaddrinfo(res);
  if (!found) {
    eprintf("can not connect to server\n");
  }
  return 0;
}