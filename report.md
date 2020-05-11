# 文件服务器实验报告

## 协议设计

见 protocol.md 。协议设计文档定义了文件下载和上传协议，规定了数据格式和服务端和客户端的行为。

## 服务端设计

服务端采用的是单进程单线程并发模型，采用 epoll 作为多路复用的机制，设计了一个状态机模型，可以并发地处理多个连接。

### 状态机设计

为了并发地处理多个连接，对于每个连接，都需要维护一个状态，表示当前与客户端通信的阶段。一共设计了如下的几种状态：

1. WaitForCommand：等待客户端的请求类型（下载还是上传）
2. WaitForName：等待客户端发送文件名
3. WaitForBodyLen：（仅上传）等待客户端发送文件大小
4. WaitForBody：（仅上传）等待客户端发送文件内容
5. SendResp：发送请求结果（上传成功、下载成功、请求失败）和文件大小（仅下载）
6. SendFile：（仅下载）向客户端发送文件内容

并且有读缓冲和写缓冲，用于解决 socket 多次写不能完成的情况。

每个状态的行为如下：

WaitForCommand：
1. 尝试读取（最多使得读缓冲长度为 1），直到读缓冲的长度为 1
2. 如果读缓冲长度为 1，取出第一个字节，判断并记录请求类型
3. 如果请求合法，转到 WaitForName 状态
4. 如果请求不合法，断开连接

WaitForName：
1. 尝试读取（最多使得读缓冲长度为 1+256），直到读缓冲的长度为 1+256
2. 如果读缓冲长度为 1+256，跳过第一个字节的请求类型，把其余 256 字节作为文件名
3. 如果当前请求是下载，则打开文件；如果打开失败，则构造请求失败的回复到写缓冲，并转到 SendResp 状态；如果打开成功，则构造下载成功的回复和文件大小到写缓冲，并转到 SendResp 状态
4. 如果当前请求是上传，则创建并打开文件；如果打开失败，记录；转到 WaitForBodyLen 状态

WaitForBodyLen（仅上传）：
1. 尝试读取（最多使得读缓冲长度为 1+256+4），直到读缓冲的长度为 1+256+4
2. 如果读缓冲长度为 1+256+4，取最后四字节为文件长度
3. 转到 WaitForBody 状态

WaitForBody（仅上传）：
1. 不断读取 socket（总共最多读取文件长度的字节），如果打开文件成功，则写入文件
2. 写完后，按照文件是否打开成功，构造上传成功或者失败的相应到写缓冲，转到 SendResp 状态

SendResp：
1. 尝试写，直到把写缓冲清空
2. 判断请求结果，如果是下载成功，则转到 SendFile 状态；如果是其他状态，则清空状态，转到 WaitForCommand 处理下一个请求

SendFile（仅下载）：
1. 不断读取文件直到 EOF ，同时写到 socket
2. 文件写完以后，清空状态，转到 WaitForCommand 处理下一个请求

### 状态设计要点

在设计状态和实现的时候，有如下几条注意的点：

1. 读 socket 的时候需要注意，不要多读，在少读的时候重试直到 EAGAIN/EWOULDBLOCK 为止，并且保持状态不变。
2. 在上传的时候，在得到文件名以后，即使此时已经知道上传会失败，也要把客户端传过来的文件内容读取再丢弃，这样才符合协议
3. 每当一个 socket 可读/可写的时候，都需要不断启动状态机，直到 EAGAIN/EWOULDBLOCK 为止，因为 epoll 的模式是 edge trigger，如果这次没有读完全，之后的新事件会丢失。
4. 服务端要保证单个用户的错误不会影响其他用户的使用；前一个请求的错误不会影响后一个请求。
5. 在客户端退出的时候也要及时回收资源。

### 代码实现

服务端代码在 server.cpp 中，另外有几个功能函数在 common.cpp 中，编译采用 CMake，方法如下：

```
mkdir build
cd build
cmake .. # 可以设置 debug/release
make
```

默认在 debug 模式下开启了 ASan，如果编译器不支持，可以在 CMakeLists 中进行修改。

编译后生成两个文件：server 和 client，分别是服务端和客户端。

服务端接受一个参数：端口。服务端会尝试 IPv4 和 IPv6 的监听：

```
$ ./server 8080
listening to 0.0.0.0:8080
listening to :::8080
```

客户端接收若干个参数，前两个参数为服务端地址和端口，之后每三个参数为一组，分别是 操作 本地路径 远端路径：

```
Usage: ./client addr port [actions]
        actions: You should specify one or more pairs of (action, local_path, remote_path) where action is one of: download and upload
```

比如，如果要上传 abc 文件到 temp；上传 abc 文件到 temp2；下载 temp2 到 temp3:

```
$ ./client :: 8080 upload abc temp upload abc temp2 download temp3 temp2
connecting to :::8080
connected!
sending upload action to server
sending remote path to server
sending file size 3 to server
reading resp from server
sending upload action to server
sending remote path to server
sending file size 3 to server
reading resp from server
sending download action to server
sending remote path to server
reading resp from server
receiving file of length 3
written to temp3
```

注意下载的时候文件顺序也是先本地后对端。