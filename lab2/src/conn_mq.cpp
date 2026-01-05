#include "conn_iface.h"
#include "common.h"
#include <mqueue.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

class MQConn : public IConn {
public:
    MQConn(mqd_t inq, mqd_t outq): in_(inq), out_(outq) {}
    ~MQConn() override {
        if (in_!=(mqd_t)-1) mq_close(in_);
        if (out_!=(mqd_t)-1) mq_close(out_);
    }
    bool send(const void* buf, size_t len, int timeout_ms) override {
        struct timespec ts = ts_after_ms(timeout_ms);
        return mq_timedsend(out_, (const char*)buf, len, 0, &ts)==0;
    }
    bool recv(void* buf, size_t len, int timeout_ms) override {
        struct timespec ts = ts_after_ms(timeout_ms);
        unsigned prio=0;
        ssize_t n = mq_timedreceive(in_, (char*)buf, len, &prio, &ts);
        return n==(ssize_t)len;
    }
private:
    mqd_t in_=(mqd_t)-1, out_=(mqd_t)-1;
};

static mqd_t mq_make(const std::string& name, bool create, bool rdonly, size_t msgsize) {
    struct mq_attr attr; memset(&attr,0,sizeof(attr));
    attr.mq_maxmsg = 32;
    attr.mq_msgsize = msgsize;
    int flags = rdonly ? O_RDONLY : O_WRONLY;
    if (create) flags |= O_CREAT;
    mqd_t q = mq_open(name.c_str(), flags, 0666, create? &attr : nullptr);
    return q;
}

IConn* create_conn_mq_host(pid_t hostpid, uint32_t cid) {
    std::string nin = mq_name_in(hostpid, cid);
    std::string nout= mq_name_out(hostpid, cid);
    // Хост: читает из in, пишет в out. Создаёт обе.
    mqd_t inq  = mq_make(nin, true, true, sizeof(Msg));
    mqd_t outq = mq_make(nout, true, false, sizeof(Msg));
    if (inq==(mqd_t)-1 || outq==(mqd_t)-1) {
        if (inq!=(mqd_t)-1) mq_unlink(nin.c_str());
        if (outq!=(mqd_t)-1) mq_unlink(nout.c_str());
        return nullptr;
    }
    return new MQConn(inq,outq);
}

IConn* create_conn_mq_client(pid_t hostpid, uint32_t cid) {
    std::string nin = mq_name_in(hostpid, cid);
    std::string nout= mq_name_out(hostpid, cid);
    // Клиент: пишет в in, читает из out
    // Открываем без O_CREAT — хост должен создать.
    mqd_t outq = mq_open(nout.c_str(), O_RDONLY);
    mqd_t inq  = mq_open(nin.c_str(), O_WRONLY);
    if (inq==(mqd_t)-1 || outq==(mqd_t)-1) {
        return nullptr;
    }
    return new MQConn(outq, inq); // внимание: вход для recv = outq
}
