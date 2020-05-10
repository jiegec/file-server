#ifndef __COMMON_H__
#define __COMMON_H__

bool tcp_nodelay(int fd);
bool so_reuseaddr(int fd);
bool nonblocking(int fd);

#endif