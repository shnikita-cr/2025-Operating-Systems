#include "conn_iface.h"
#include "common.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>

class SockConn : public IConn {
public:
    explicit SockConn(int fd): fd_(fd) {}
    ~SockConn() override { if (fd_>=0) close(fd_); }
    bool send(const void* buf, size_t len, int timeout_ms) override {
        // Заголовок в network byte order (5*uint32_t)
        if (len != sizeof(Msg)) return false;
        Msg net = *(const Msg*)buf;
        net.type      = htonl(net.type);
        net.client_id = htonl(net.client_id);
        net.round_no  = htonl(net.round_no);
        net.value     = htonl(net.value);
        net.state     = htonl(net.state);
        struct pollfd pfd{fd_, POLLOUT, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr<=0) return false;
        ssize_t n = ::send(fd_, &net, sizeof(net), MSG_NOSIGNAL);
        return n==(ssize_t)sizeof(net);
    }
    bool recv(void* buf, size_t len, int timeout_ms) override {
        if (len != sizeof(Msg)) return false;
        struct pollfd pfd{fd_, POLLIN, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr<=0) return false;
        Msg net{};
        ssize_t n = ::recv(fd_, &net, sizeof(net), MSG_WAITALL);
        if (n!=(ssize_t)sizeof(net)) return false;
        Msg host = net;
        host.type      = ntohl(net.type);
        host.client_id = ntohl(net.client_id);
        host.round_no  = ntohl(net.round_no);
        host.value     = ntohl(net.value);
        host.state     = ntohl(net.state);
        memcpy(buf, &host, sizeof(host));
        return true;
    }
private:
    int fd_=-1;
};

int sock_listen_create(const char* ip, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd<0) return -1;
    int one=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    inet_pton(AF_INET, ip, &sa.sin_addr);
    if (bind(fd, (sockaddr*)&sa, sizeof(sa))<0) { close(fd); return -1; }
    if (listen(fd, 64)<0) { close(fd); return -1; }
    return fd;
}

IConn* create_conn_sock_host_from_fd(int fd) {
    return new SockConn(fd);
}

IConn* create_conn_sock_client_connect(const char* ip, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd<0) return nullptr;
    sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr)!=1) { close(fd); return nullptr; }
    if (connect(fd, (sockaddr*)&sa, sizeof(sa))<0) { close(fd); return nullptr; }
    return new SockConn(fd);
}
