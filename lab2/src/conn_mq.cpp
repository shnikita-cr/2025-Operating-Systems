#include "conn_iface.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

class MQConn : public IConn {
public:
    MQConn(mqd_t inq, mqd_t outq) : in_(inq), out_(outq) {}

    ~MQConn() override {
        if (in_ != (mqd_t) - 1) mq_close(in_);
        if (out_ != (mqd_t) - 1) mq_close(out_);
    }

    bool send(const void *buf, size_t len, int timeout_ms) override {
        struct timespec ts = ts_after_ms(timeout_ms);
        return mq_timedsend(out_, (const char *) buf, len, 0, &ts) == 0;
    }

    bool recv(void *buf, size_t len, int timeout_ms) override {
        struct timespec ts = ts_after_ms(timeout_ms);
        unsigned prio = 0;
        ssize_t n = mq_timedreceive(in_, (char *) buf, len, &prio, &ts);
        return n == (ssize_t) len;
    }

private:
    mqd_t in_ = (mqd_t) - 1;
    mqd_t out_ = (mqd_t) - 1;
};

static mqd_t mq_open_create_with_fallback(const std::string &name, int open_mode, size_t msgsize) {
    mkdir("/dev/mqueue", 0777);
    static const long candidates[] = {16, 10, 8, 6, 4, 2, 1};

    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_msgsize = (long) msgsize;

    for (long maxmsg: candidates) {
        attr.mq_maxmsg = maxmsg;
        mqd_t q = mq_open(name.c_str(), open_mode | O_CREAT, 0666, &attr);
        if (q != (mqd_t) - 1) return q;
        if (errno != EINVAL) break;
    }

    return (mqd_t) - 1;
}

IConn *create_conn_mq_host(pid_t hostpid, uint32_t cid) {
    std::string nin = mq_name_in(hostpid, cid);
    std::string nout = mq_name_out(hostpid, cid);


    mq_unlink(nin.c_str());
    mq_unlink(nout.c_str());


    mqd_t inq = mq_open_create_with_fallback(nin, O_RDWR, sizeof(Msg));
    mqd_t outq = mq_open_create_with_fallback(nout, O_RDWR, sizeof(Msg));

    if (inq == (mqd_t) - 1 || outq == (mqd_t) - 1) {
        if (inq != (mqd_t) - 1) mq_close(inq);
        if (outq != (mqd_t) - 1) mq_close(outq);
        mq_unlink(nin.c_str());
        mq_unlink(nout.c_str());
        return nullptr;
    }

    return new MQConn(inq, outq);
}

static mqd_t mq_open_retry(const std::string &name, int flags) {
    mkdir("/dev/mqueue", 0777);


    for (int i = 0; i < 100; ++i) {
        mqd_t q = mq_open(name.c_str(), flags);
        if (q != (mqd_t) - 1) return q;
        if (errno != ENOENT) break;
        usleep(10 * 1000);
    }
    return (mqd_t) - 1;
}

IConn *create_conn_mq_client(pid_t hostpid, uint32_t cid) {
    std::string nin = mq_name_in(hostpid, cid);
    std::string nout = mq_name_out(hostpid, cid);


    mqd_t outq = mq_open_retry(nout, O_RDWR);
    mqd_t inq = mq_open_retry(nin, O_RDWR);

    if (inq == (mqd_t) - 1 || outq == (mqd_t) - 1) {
        if (inq != (mqd_t) - 1) mq_close(inq);
        if (outq != (mqd_t) - 1) mq_close(outq);
        return nullptr;
    }


    return new MQConn(outq, inq);
}
