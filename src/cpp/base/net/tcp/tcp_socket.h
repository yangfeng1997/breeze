// TCP 专有的 socket 选项与判断。字节流共用的部分见 stream/stream_socket.h。
//
// 约定：返回 int 的函数返回 errno（0 = 成功），不抛异常。

#pragma once

namespace breeze
{

namespace tcp
{

// TCP keepalive 参数。只开 SO_KEEPALIVE 而不设参数时内核默认 7200s，等于没开，所以三个参数一起设。
struct KeepAlive
{
    int idle_s = 60;
    int interval_s = 10;
    int count = 3;
    int user_timeout_ms = 0; // TCP_USER_TIMEOUT：已发出的数据多久得不到确认就断开；0 = 不设
};

int SetNoDelay(int fd);
int SetReuseAddr(int fd);
int SetKeepAlive(int fd, const KeepAlive& ka);
int SetUserTimeout(int fd, int ms); // TCP_USER_TIMEOUT，不开 keepalive 时单独用

// 本地与对端的地址、端口完全相同：loopback 重连时 TCP 同时打开造成的"连到了自己"。
bool IsSelfConnect(int fd);

} // namespace tcp

} // namespace breeze
