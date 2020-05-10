#include "common.h"
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

enum State {
  WaitForCommand,
  WaitForName,
  WaitForBodyLen,
  WaitForBody,
  SendResp,
  SendFile,
};
enum Command { Download, Upload };

struct SocketState {
  int fd;
  int is_listen; // true for listen socket, false for client socket
  State state;
  Command current_command;
  std::vector<uint8_t> read_buffer;
  std::string name;
  // resp header
  std::vector<uint8_t> write_buffer;
  int buffer_written;
  // Download only
  int file_fd;
  // Upload only
  uint32_t body_len;
  uint32_t written_len;
};

int read_exact(struct SocketState &state, size_t len) {
  size_t read_len = 0;
  char buffer[128];
  while (read_len < len) {
    int res = read(state.fd, buffer, std::min(sizeof(buffer), len - read_len));
    if (res == 0) {
      break;
    }
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

  // ignore SIGPIPE because we use epoll to handle it
  signal(SIGPIPE, SIG_IGN);

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

    // set reuseaddr
    if (!so_reuseaddr(fd)) {
      // fail
      close(fd);
      continue;
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
    if (!nonblocking(fd)) {
      // error
      close(fd);
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
  freeaddrinfo(res);

  if (state.size() == 0) {
    eprintf("unable to bind\n");
    return 1;
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
      }

      if (state.find(events[i].data.fd) != state.cend()) {
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
            if (!nonblocking(fd)) {
              // error
              close(fd);
              continue;
            }
            tcp_nodelay(fd);

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
            event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
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
          // try to read/write as much as possible until EAGAIN/EWOUDLBLOCK
          bool invalid = false;
          for (;;) {
            printf("state at %d\n", s.state);
            if (s.state == State::WaitForCommand) {
              if (read_exact(s, 1) == 1) {
                if (s.read_buffer[0] == 0x0) {
                  s.current_command = Command::Download;
                } else if (s.read_buffer[0] == 0x1) {
                  s.current_command = Command::Upload;
                } else {
                  invalid = true;
                  break;
                }
                s.state = State::WaitForName;
              } else {
                // can't read more
                break;
              }
            }

            if (s.state == State::WaitForName) {
              int expected_len = 256 + 1;
              read_exact(s, expected_len - s.read_buffer.size());
              if (s.read_buffer.size() == expected_len) {
                // got name
                std::vector<char> temp;
                temp.assign(&s.read_buffer[1], &s.read_buffer[expected_len]);
                // append NUL if length of name is 256 bytes
                temp.push_back(0);
                s.name = temp.data();
                if (s.current_command == Command::Upload) {
                  // upload
                  s.state = State::WaitForBodyLen;
                  printf("user wants to upload: %s\n", s.name.c_str());
                } else {
                  // download
                  printf("user wants to download: %s\n", s.name.c_str());
                  int fd = open(s.name.c_str(), O_RDONLY);
                  if (fd < 0) {
                    eprintf("unable to open file: %s\n", s.name.c_str());

                    // error handling
                    s.write_buffer.clear();
                    // error resp
                    s.write_buffer.push_back(0x00);
                    s.buffer_written = 0;
                    s.state = State::SendResp;
                  } else {
                    struct stat st;
                    fstat(fd, &st);
                    s.file_fd = fd;
                    s.state = State::SendResp;
                    s.write_buffer.clear();
                    // download resp
                    s.write_buffer.push_back(0x02);
                    // length in big endian
                    s.write_buffer.push_back((st.st_size >> 24) & 0xFF);
                    s.write_buffer.push_back((st.st_size >> 16) & 0xFF);
                    s.write_buffer.push_back((st.st_size >> 8) & 0xFF);
                    s.write_buffer.push_back((st.st_size >> 0) & 0xFF);
                    s.buffer_written = 0;
                  }
                }
              } else {
                // can't read more
                break;
              }
            }

            if (s.state == State::WaitForBodyLen) {
              int expected_len = 4 + 256 + 1;
              read_exact(s, expected_len - s.read_buffer.size());
              if (s.read_buffer.size() == expected_len) {
                // got body len
                s.body_len = ntohl(*(uint32_t *)&s.read_buffer[256 + 1]);
                s.written_len = 0;
                s.state = State::WaitForBody;
              } else {
                // can't read more
                break;
              }
            }

            if (s.state == State::WaitForBody) {
              if (s.body_len == s.written_len) {
                // done, go to next request
                s.state = State::WaitForCommand;
              } else {
                // TODO
              }
            }

            if (s.state == State::SendResp) {
              // send write_buffer to remote
              while (s.buffer_written < s.write_buffer.size()) {
                int written = write(s.fd, &s.write_buffer[s.buffer_written],
                                    s.write_buffer.size() - s.buffer_written);
                if (written < 0) {
                  if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                  }
                  perror("write");
                  break;
                  // TODO: error handling
                }
                s.buffer_written += written;
              }

              if (s.buffer_written == s.write_buffer.size()) {
                // s.read_buffer.clear();
                if (s.write_buffer.size() != 1) {
                  // send file
                  s.state = State::SendFile;
                } else {
                  // finish
                  s.state = State::WaitForCommand;
                }
              }
            }

            if (s.state == State::SendFile) {
              for (;;) {
                int res = sendfile(s.fd, s.file_fd, NULL, 0xFFFFFFFF);
                if (res == 0) {
                  // EOF
                  printf("complete sending file to client\n");
                  close(s.file_fd);
                  s.state = State::WaitForCommand;
                  s.read_buffer.clear();
                  break;
                } else if (res < 0) {
                  if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                  }
                  // error handling
                  break;
                }
              }
            }
          }

          // got invalid data
          if (invalid) {
            printf("client sent invalid data\n");
            state.erase(events[i].data.fd);
            close(events[i].data.fd);
          }
        }
      }

      if (events[i].events & EPOLLRDHUP) {
        // remote closed connection
        printf("remote closed connection\n");
        state.erase(events[i].data.fd);
        close(events[i].data.fd);
      }
    }
  }

  return 0;
}