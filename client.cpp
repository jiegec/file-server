#include "common.h"
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#define eprintf(...) fprintf(stderr, __VA_ARGS__)

int read_exact(int fd, char *buffer, size_t len) {
  size_t read_len = 0;
  while (read_len < len) {
    int res = read(fd, &buffer[read_len], len - read_len);
    if (res < 0) {
      return -1;
    }
    read_len += res;
  }
  return read_len;
}

int write_exact(int fd, char *buffer, size_t len) {
  size_t write_len = 0;
  while (write_len < len) {
    int res = write(fd, &buffer[write_len], len - write_len);
    if (res < 0) {
      return -1;
    }
    write_len += res;
  }
  return write_len;
}

int main(int argc, char *argv[]) {
  if (argc < 6 || (argc % 2) == 1) {
    eprintf("Usage: %s addr port download|upload local_path remote_path ... "
            "[local_path] [remote_path]\n\tYou can specify one or more pairs "
            "of (local_path, remote_path)",
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

  int ret = 0;

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
    // no delay
    tcp_nodelay(fd);
    // 3s recv timeout
    so_recv_timeout(fd, 3000000);

    printf("connected!\n");
    found = true;

    if (strcmp(argv[3], "download") == 0) {
      // download
      // begin from argv[4]
      for (int offset = 4; offset < argc; offset += 2) {
        int file_fd = open(argv[offset], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (file_fd < 0) {
          eprintf("unable to open %s\n", argv[offset]);
          perror("open");
          continue;
        }

        // req
        char action = 0x0;
        printf("sending download action to server\n");
        if (write_exact(fd, &action, 1) != 1) {
          perror("write");
          ret = 1;
          goto quit;
        }

        // copy to zero-init array to avoid leaking
        char name[256] = {0};
        char *remote_path = argv[offset + 1];
        if (strlen(remote_path) > 256) {
          eprintf("file name too long!\n");
          ret = 1;
          goto quit;
        }
        memcpy(name, remote_path, strlen(remote_path));
        printf("sending remote path to server\n");
        if (write_exact(fd, name, 256) != 256) {
          perror("write");
          ret = 1;
          goto quit;
        }

        // resp
        char resp = 0x0;
        printf("reading resp from server\n");
        if (read_exact(fd, &resp, 1) != 1) {
          perror("read");
          ret = 1;
          goto quit;
        }
        if (resp == 0x0) {
          eprintf("server resp: download failed\n");
          close(file_fd);
          continue;
        } else if (resp == 0x2) {
          uint32_t length;
          if (read_exact(fd, (char *)&length, sizeof(length)) < 0) {
            perror("read");
            ret = 1;
            goto quit;
          }
          length = ntohl(length);
          printf("receiving file of length %d\n", length);
          uint32_t read_len = 0;
          char buffer[128];
          while (read_len < length) {
            int res =
                read(fd, buffer,
                     std::min((uint32_t)sizeof(buffer), length - read_len));
            if (res < 0) {
              perror("read");
              break;
            }
            read_len += res;
            uint32_t write_len = 0;
            while (write_len < res) {
              int res2 = write(file_fd, buffer, res - write_len);
              if (res2 < 0) {
                perror("write");
                break;
              }
              write_len += res2;
            }
          }
          printf("written to %s\n", argv[4]);
          close(file_fd);
        }
      }
    } else if (strcmp(argv[3], "upload") == 0) {
      // upload
      // begin from argv[4]
      for (int offset = 4; offset < argc; offset += 2) {
        int file_fd = open(argv[offset], O_RDONLY);
        if (file_fd < 0) {
          eprintf("unable to open %s\n", argv[offset]);
          perror("open");
          continue;
        }

        // req
        char action = 0x01;
        printf("sending upload action to server\n");
        if (write_exact(fd, &action, 1) != 1) {
          perror("write");
          ret = 1;
          goto quit;
        }

        // copy to zero-init array to avoid leaking
        char name[256] = {0};
        char *remote_path = argv[offset + 1];
        if (strlen(remote_path) > 256) {
          eprintf("file name too long!\n");
          ret = 1;
          goto quit;
        }
        memcpy(name, remote_path, strlen(remote_path));
        printf("sending remote path to server\n");
        if (write_exact(fd, name, 256) != 256) {
          perror("write");
          ret = 1;
          goto quit;
        }

        // send file size
        struct stat st;
        fstat(file_fd, &st);
        char file_len[4];
        // big endian
        file_len[0] = st.st_size >> 24;
        file_len[1] = st.st_size >> 16;
        file_len[2] = st.st_size >> 8;
        file_len[3] = st.st_size;
        printf("sending file size %d to server\n", st.st_size);
        if (write_exact(fd, file_len, 4) != 4) {
          perror("write");
          ret = 1;
          goto quit;
        }

        // sending file content
        uint32_t length = st.st_size;
        uint32_t read_len = 0;
        char buffer[128];
        while (read_len < length) {
          int res = read(file_fd, buffer,
                         std::min((uint32_t)sizeof(buffer), length - read_len));
          if (res < 0) {
            perror("read");
            ret = 1;
            goto quit;
          }
          read_len += res;
          uint32_t write_len = 0;
          while (write_len < res) {
            int res2 = write(fd, buffer, res - write_len);
            if (res2 < 0) {
              perror("write");
              ret = 1;
              goto quit;
            }
            write_len += res2;
          }
        }
        close(file_fd);

        // resp
        char resp = 0x0;
        printf("reading resp from server\n");
        if (read_exact(fd, &resp, 1) != 1) {
          perror("read");
          ret = 1;
          goto quit;
        }

        if (resp == 0x0) {
          eprintf("server resp: download failed\n");
          continue;
        }
      }
    } else {
      printf("unsupported action: %s\n", argv[3]);
    }

    close(fd);
    break;
  }
quit:
  freeaddrinfo(res);
  if (!found) {
    eprintf("can not connect to server\n");
  }
  return ret;
}