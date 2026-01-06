#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <semaphore.h>
#include <signal.h>

struct Msg {
    uint32_t type;      // 1..8
    uint32_t client_id; // assigned by host
    uint32_t round_no;
    uint32_t value;     // number
    uint32_t state;     // 0=unknown, 1=alive, 2=dead
    char nick[32];  // ASCIIZ
    uint8_t reserved[12]; // padding to 64 bytes (mq_msgsize requirement)
};
static_assert(sizeof(Msg) == 64, "Msg must be 64 bytes");

enum MsgType : uint32_t {
    HELLO = 1, HELLO_ACK = 2,
    ROUND_START = 3, TURN = 4, RESULT = 5,
    GAME_OVER = 6, PING = 7, PONG = 8
};

enum ClientState : uint32_t {
    ST_UNKNOWN = 0, ST_ALIVE = 1, ST_DEAD = 2
};

struct CliArgsHost {
    std::string mode;       // mq|fifo|sock
    std::string log_dir = "/etc/logs";
    std::string nick = "wolf";
    std::string addr = "0.0.0.0";
    int port = 5555;
    int rounds = -1;          // -1 бесконечно
    int timeout_ms = 5000;
};

struct CliArgsClient {
    pid_t host_pid = 0;
    std::string log_dir = "/etc/logs";
    std::string nick = "kid";
    std::string host_addr = "127.0.0.1";
    int host_port = 5555;
    int timeout_ms = 5000;
};

class Logger {
public:
    explicit Logger(const std::string &path, const char *who, const char *mode = nullptr);

    ~Logger();

    void log(const std::string &s);

    void err(const std::string &s);

private:
    int fd_ = -1;
    std::string who_;
    std::string mode_;
};

uint64_t epoch_sec();

bool ensure_dir(const std::string &path, int mode = 0777);

bool parse_host_args(int argc, char **argv, CliArgsHost &out);

bool parse_client_args(int argc, char **argv, CliArgsClient &out);

std::string sem_name_for(pid_t hostpid, uint32_t cid);

std::string mq_name_in(pid_t hostpid, uint32_t cid);

std::string mq_name_out(pid_t hostpid, uint32_t cid);

std::string fifo_base(pid_t hostpid);

std::string fifo_name_in(pid_t hostpid, uint32_t cid);

std::string fifo_name_out(pid_t hostpid, uint32_t cid);

timespec ts_after_ms(int ms);

void rng_seed();

int rng_uniform(int lo, int hi);

int try_read_number_3s_range(int lo, int hi, bool sleep_when_no_tty);

int try_read_number_3s();


// Signal utils
void install_sigusr1_host();

void push_pending_client(pid_t pid);

bool pop_pending_client(pid_t &pid_out);

void write_host_pid_file(pid_t pid);
