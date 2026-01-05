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
    FIFOConn(int in_fd, int out_fd) : in_(in_fd), out_(out_fd) {}

    ~FIFOConn() override {
        if (in_ >= 0) close(in_);
        if (out_ >= 0) close(out_);
    }

    bool send(const void *buf, size_t len, int timeout_ms) override {
        const uint8_t *p = (const uint8_t *) buf;
        size_t left = len;
        while (left > 0) {
            struct pollfd pfd{out_, POLLOUT, 0};
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;
            ssize_t n = write(out_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += (size_t) n;
            left -= (size_t) n;
        }
        return true;
    }

    bool recv(void *buf, size_t len, int timeout_ms) override {
        uint8_t *p = (uint8_t *) buf;
        size_t left = len;
        while (left > 0) {
            struct pollfd pfd{in_, POLLIN, 0};
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr <= 0) return false;
            ssize_t n = read(in_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) continue;
                return false;
            }
            if (n == 0) return false;
            p += (size_t) n;
            left -= (size_t) n;
        }
        return true;
    }

private:
    int in_ = -1, out_ = -1;
};

IConn *create_conn_fifo_host(pid_t hostpid, uint32_t cid) {
    std::string base = fifo_base(hostpid);
    ensure_dir(base, 0777);
    std::string pin = fifo_name_in(hostpid, cid);
    std::string pout = fifo_name_out(hostpid, cid);
    mkfifo(pin.c_str(), 0666);
    mkfifo(pout.c_str(), 0666);

    int in_fd = open(pin.c_str(), O_RDWR | O_NONBLOCK);
    int out_fd = open(pout.c_str(), O_RDWR | O_NONBLOCK);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) close(in_fd);
        if (out_fd >= 0) close(out_fd);
        return nullptr;
    }

    return new FIFOConn(in_fd, out_fd);
}

IConn *create_conn_fifo_client(pid_t hostpid, uint32_t cid) {
    std::string pin = fifo_name_in(hostpid, cid);
    std::string pout = fifo_name_out(hostpid, cid);

    int out_fd = open(pin.c_str(), O_RDWR | O_NONBLOCK);
    int in_fd = open(pout.c_str(), O_RDWR | O_NONBLOCK);
    if (in_fd < 0 || out_fd < 0) {
        if (in_fd >= 0) close(in_fd);
        if (out_fd >= 0) close(out_fd);
        return nullptr;
    }
    return new FIFOConn(in_fd, out_fd);
}
