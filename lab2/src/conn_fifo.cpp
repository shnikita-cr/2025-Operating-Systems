#include "conn_iface.h"
#include "common.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

class FIFOConn : public IConn {
public:
    FIFOConn(int in_fd, int out_fd): in_(in_fd), out_(out_fd) {}
    ~FIFOConn() override {
        if (in_>=0) close(in_);
        if (out_>=0) close(out_);
    }
    bool send(const void* buf, size_t len, int timeout_ms) override {
        struct pollfd pfd{out_, POLLOUT, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr<=0) return false;
        ssize_t n = write(out_, buf, len);
        return n==(ssize_t)len;
    }
    bool recv(void* buf, size_t len, int timeout_ms) override {
        struct pollfd pfd{in_, POLLIN, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr<=0) return false;
        ssize_t n = read(in_, buf, len);
        return n==(ssize_t)len;
    }
private:
    int in_=-1, out_=-1;
};

IConn* create_conn_fifo_host(pid_t hostpid, uint32_t cid) {
    std::string base = fifo_base(hostpid);
    ensure_dir(base, 0777);
    std::string pin = fifo_name_in(hostpid, cid);
    std::string pout= fifo_name_out(hostpid, cid);
    mkfifo(pin.c_str(), 0666);
    mkfifo(pout.c_str(),0666);
    // Чтобы не блокироваться — открываем O_RDWR с обеих сторон у хоста
    int in_fd  = open(pin.c_str(),  O_RDWR|O_NONBLOCK);
    int out_fd = open(pout.c_str(), O_RDWR|O_NONBLOCK);
    if (in_fd<0 || out_fd<0) {
        if (in_fd>=0) close(in_fd);
        if (out_fd>=0) close(out_fd);
        return nullptr;
    }
    // В нашей логике: host читает из pin (C->H), пишет в pout (H->C)
    return new FIFOConn(in_fd, out_fd);
}

IConn* create_conn_fifo_client(pid_t hostpid, uint32_t cid) {
    std::string pin = fifo_name_in(hostpid, cid);
    std::string pout= fifo_name_out(hostpid, cid);
    // Клиент: пишет в pin, читает из pout — открываем RDWR для симметрии
    int out_fd = open(pin.c_str(),  O_RDWR|O_NONBLOCK); // send -> pin
    int in_fd  = open(pout.c_str(), O_RDWR|O_NONBLOCK); // recv <- pout
    if (in_fd<0 || out_fd<0) {
        if (in_fd>=0) close(in_fd);
        if (out_fd>=0) close(out_fd);
        return nullptr;
    }
    return new FIFOConn(in_fd, out_fd);
}
