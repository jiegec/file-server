#ifndef __COMMON_H__
#define __COMMON_H__

bool tcp_nodelay(int fd);
bool so_reuseaddr(int fd);
bool nonblocking(int fd);
bool so_recv_timeout(int fd, int usec);

#endif