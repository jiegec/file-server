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

enum State {
  WaitForCommand,
  WaitForNameLen,
  WaitForName,
  WaitForBodyLen,
  WaitForBody
};
enum Command { Download, Upload };

struct SocketState {
  int fd;
  int is_listen; // true for listen socket, false for client socket
  State state;
  Command current_command;
  std::vector<uint8_t> read_buffer;
  int name_len;
  std::string name;
  uint32_t body_len;
};

int read_exact(struct SocketState &state, size_t len) {
  size_t read_len = 0;
  char buffer[128];
  while (read_len < len) {
    int res = read(state.fd, buffer, std::min(sizeof(buffer), len - read_len));
    if (res < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return -1;
    }
    state.read_buffer.insert(state.read_buffer.cend(), buffer, buffer + res);
    read_len += res;
  }
  return read_len;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    eprintf("Usage: %s port\n", argv[0]);
    return 1;
  }

  // fd states
  std::map<int, SocketState> state;

  // setup epoll
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    return 1;
  }

  // bind to port
  char *port = argv[1];
  int error;
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  error = getaddrinfo(NULL, port, &hints, &res);
  if (error != 0 || res == NULL) {
    eprintf("getaddrinfo: %s\n", gai_strerror(error));
    return 1;
  }
  for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
    int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      perror("socket");
      continue;
    }

    // set v6only when needed
    if (p->ai_family == AF_INET6) {
      int on = 1;
      if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0) {
        close(fd);
        perror("setsockopt");
        continue;
      }
    }

    // bind
    if (bind(fd, p->ai_addr, p->ai_addrlen) < 0) {
      close(fd);
      perror("bind");
      continue;
    }

    // listen
    if (listen(fd, SOMAXCONN) < 0) {
      close(fd);
      perror("listen");
      continue;
    }

    // set non blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      close(fd);
      perror("fcntl");
      continue;
    }
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
      close(fd);
      perror("fcntl");
      continue;
    }

    // add to epoll
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
      close(fd);
      perror("epoll_ctl");
      continue;
    }

    // print info
    char hbuf[NI_MAXHOST];
    char sbuf[NI_MAXSERV];
    error = getnameinfo(p->ai_addr, p->ai_addrlen, hbuf, sizeof(hbuf), sbuf,
                        sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
    if (error != 0) {
      eprintf("getnameinfo: %s\n", gai_strerror(error));
      return 1;
    }
    printf("listening to %s:%s\n", hbuf, sbuf);
    SocketState ss;
    ss.fd = fd;
    ss.is_listen = true;
    state[fd] = ss;
  }

  int max_event_count = 4096;
  struct epoll_event *events = (struct epoll_event *)malloc(
      max_event_count * sizeof(struct epoll_event));
  memset(events, 0, max_event_count * sizeof(struct epoll_event));

  // event loop
  while (true) {
    int count = epoll_wait(epoll_fd, events, max_event_count, -1);
    for (int i = 0; i < count; i++) {
      if (events[i].events & EPOLLERR | events[i].events & EPOLLHUP) {
        eprintf("fd %d got error\n", events[i].data.fd);
        close(events[i].data.fd);
        continue;
      } else if (events[i].events & EPOLLIN) {
        SocketState &s = state[events[i].data.fd];
        if (s.is_listen) {
          // accept all incoming sockets
          for (;;) {
            struct sockaddr_storage in_addr;
            memset(&in_addr, 0, sizeof(in_addr));
            socklen_t in_len = sizeof(in_addr);
            int fd =
                accept(events[i].data.fd, (struct sockaddr *)&in_addr, &in_len);
            if (fd < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no more socket to accept
                break;
              } else {
                perror("accept");
                break;
              }
            }

            // set non blocking
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) {
              close(fd);
              perror("fcntl");
              continue;
            }
            flags |= O_NONBLOCK;
            if (fcntl(fd, F_SETFL, flags) < 0) {
              close(fd);
              perror("fcntl");
              continue;
            }

            // print info
            char hbuf[NI_MAXHOST];
            char sbuf[NI_MAXSERV];
            error = getnameinfo((struct sockaddr *)&in_addr, in_len, hbuf,
                                sizeof(hbuf), sbuf, sizeof(sbuf),
                                NI_NUMERICHOST | NI_NUMERICSERV);
            if (error != 0) {
              eprintf("getnameinfo: %s\n", gai_strerror(error));
              close(fd);
              continue;
            }
            printf("get connection from %s:%s\n", hbuf, sbuf);

            // add to epoll
            struct epoll_event event;
            event.data.fd = fd;
            event.events = EPOLLIN | EPOLLET;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
              close(fd);
              perror("epoll_ctl");
              continue;
            }

            // add to state
            SocketState ss;
            ss.fd = fd;
            ss.is_listen = false;
            ss.state = State::WaitForCommand;
            state[fd] = ss;
          }
        } else {
          // client
        }
      }
    }
  }

  freeaddrinfo(res);
  return 0;
}