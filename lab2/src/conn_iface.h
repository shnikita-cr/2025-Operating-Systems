#pragma once

#include "common.h"
#include <cstddef>

class IConn {
public:
    virtual ~IConn() {}

    virtual bool send(const void *buf, size_t len, int timeout_ms) = 0;

    virtual bool recv(void *buf, size_t len, int timeout_ms) = 0;
};


IConn *create_conn_mq_host(pid_t hostpid, uint32_t cid);

IConn *create_conn_mq_client(pid_t hostpid, uint32_t cid);


IConn *create_conn_fifo_host(pid_t hostpid, uint32_t cid);

IConn *create_conn_fifo_client(pid_t hostpid, uint32_t cid);


int sock_listen_create(const char *ip, int port);

IConn *create_conn_sock_host_from_fd(int fd);

IConn *create_conn_sock_client_connect(const char *ip, int port);
