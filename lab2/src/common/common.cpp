#include "common.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <deque>
#include <iostream>
#include <mutex>

static std::mutex gq_mu;
static std::deque<pid_t> gq;

static void sigusr1_handler(int sig, siginfo_t *si, void * /*u*/) {
    if (sig == SIGUSR1 && si) {
        pid_t cpid = si->si_pid;
        std::lock_guard<std::mutex> lk(gq_mu);
        gq.push_back(cpid);
    }
}

void install_sigusr1_host() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigusr1_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
}

void push_pending_client(pid_t pid) {
    std::lock_guard<std::mutex> lk(gq_mu);
    gq.push_back(pid);
}

bool pop_pending_client(pid_t &pid_out) {
    std::lock_guard<std::mutex> lk(gq_mu);
    if (gq.empty()) return false;
    pid_out = gq.front();
    gq.pop_front();
    return true;
}

static void write_line_fd(int fd, const std::string &line) {
    if (fd < 0) return;
    ::write(fd, line.c_str(), line.size());
    ::write(fd, "\n", 1);
}

uint64_t epoch_sec() {
    return (uint64_t) time(nullptr);
}

static std::string sys_time_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    time_t sec = (time_t) ts.tv_sec;
    struct tm tmv;
    localtime_r(&sec, &tmv);

    char buf[64];
    const int ms = (int) (ts.tv_nsec / 1000000L);
    snprintf(
            buf,
            sizeof(buf),
            "%04d-%02d-%02d %02d:%02d:%02d.%03d",
            tmv.tm_year + 1900,
            tmv.tm_mon + 1,
            tmv.tm_mday,
            tmv.tm_hour,
            tmv.tm_min,
            tmv.tm_sec,
            ms
    );
    return std::string(buf);
}

Logger::Logger(const std::string &path, const char *who, const char *mode)
        : who_(who ? who : ""), mode_(mode ? mode : "") {
    if (path.empty()) return;


    auto pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) {
        ensure_dir(path.substr(0, pos), 0777);
    }

    fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
}

Logger::~Logger() {
    if (fd_ >= 0) ::close(fd_);
}

void Logger::log(const std::string &s) {
    std::string line;
    line.reserve(256);

    line += sys_time_now();
    if (!mode_.empty()) line += " [" + mode_ + "]";
    line += " [" + who_ + "] ";
    line += s;

    write_line_fd(fd_, line);
    std::cout << line << std::endl;
}

void Logger::err(const std::string &s) {
    std::string line;
    line.reserve(256);

    line += sys_time_now();
    if (!mode_.empty()) line += " [" + mode_ + "]";
    line += " [" + who_ + "] ERROR: ";
    line += s;

    write_line_fd(fd_, line);
    std::cerr << line << std::endl;
}

bool ensure_dir(const std::string &path, int mode) {
    if (path.empty()) return false;


    std::string cur;
    cur.reserve(path.size());


    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        cur = "/";
        i = 1;
    }

    while (i < path.size()) {

        while (i < path.size() && path[i] == '/') i++;
        if (i >= path.size()) break;


        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;
        std::string seg = path.substr(i, j - i);
        if (!seg.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += seg;

            struct stat st;
            if (stat(cur.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) return false;
            } else {
                if (mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
            }
        }
        i = j;
    }

    return true;
}

bool parse_host_args(int argc, char **argv, CliArgsHost &out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) out.mode = argv[++i];
        else if (a == "--log-dir" && i + 1 < argc) out.log_dir = argv[++i];
        else if (a == "--rounds" && i + 1 < argc) out.rounds = std::stoi(argv[++i]);
        else if (a == "--timeout-ms" && i + 1 < argc) out.timeout_ms = std::stoi(argv[++i]);
        else if (a == "--nick" && i + 1 < argc) out.nick = argv[++i];
        else if (a == "--addr" && i + 1 < argc) out.addr = argv[++i];
        else if (a == "--port" && i + 1 < argc) out.port = std::stoi(argv[++i]);
    }
    return true;
}

bool parse_client_args(int argc, char **argv, CliArgsClient &out) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--host-pid" && i + 1 < argc) out.host_pid = (pid_t) std::stoi(argv[++i]);
        else if (a == "--log-dir" && i + 1 < argc) out.log_dir = argv[++i];
        else if (a == "--nick" && i + 1 < argc) out.nick = argv[++i];
        else if (a == "--host-addr" && i + 1 < argc) out.host_addr = argv[++i];
        else if (a == "--host-port" && i + 1 < argc) out.host_port = std::stoi(argv[++i]);
        else if (a == "--timeout-ms" && i + 1 < argc) out.timeout_ms = std::stoi(argv[++i]);
    }
    return out.host_pid > 0;
}

std::string sem_name_for(pid_t hostpid, uint32_t cid) {
    return "/wolf_" + std::to_string(hostpid) + "_c" + std::to_string(cid) + "_step";
}

std::string mq_name_in(pid_t hostpid, uint32_t cid) {
    return "/wolf_" + std::to_string(hostpid) + "_c" + std::to_string(cid) + "_in";
}

std::string mq_name_out(pid_t hostpid, uint32_t cid) {
    return "/wolf_" + std::to_string(hostpid) + "_c" + std::to_string(cid) + "_out";
}

std::string fifo_base(pid_t hostpid) {
    return "/tmp/wolf_" + std::to_string(hostpid);
}

std::string fifo_name_in(pid_t hostpid, uint32_t cid) {
    return fifo_base(hostpid) + "/c" + std::to_string(cid) + "_in";
}

std::string fifo_name_out(pid_t hostpid, uint32_t cid) {
    return fifo_base(hostpid) + "/c" + std::to_string(cid) + "_out";
}

timespec ts_after_ms(int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns = ts.tv_nsec + (long) ms * 1000000L;
    ts.tv_sec += ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    return ts;
}

void rng_seed() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned seed = (unsigned) (ts.tv_nsec ^ ts.tv_sec ^ getpid());
    srandom(seed);
}

int rng_uniform(int lo, int hi) {
    if (hi <= lo) return lo;
    long range = (long) hi - lo + 1;
    long r = random() % range;
    return lo + (int) r;
}

int try_read_number_3s_range(int lo, int hi, bool sleep_when_no_tty) {
    if (hi < lo) return -1;


    if (!isatty(STDIN_FILENO)) {
        if (!sleep_when_no_tty) return -1;

        struct timespec req;
        req.tv_sec = 3;
        req.tv_nsec = 0;
        while (nanosleep(&req, &req) != 0 && errno == EINTR) {

        }
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 3000);
    if (pr > 0 && (pfd.revents & POLLIN)) {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0) {
            int v = 0;
            if (sscanf(buf, "%d", &v) == 1 && v >= lo && v <= hi) return v;
        }
    }
    return -1;
}

int try_read_number_3s() {
    return try_read_number_3s_range(1, 100, true);
}


void write_host_pid_file(pid_t pid) {
    ensure_dir("/app/run", 0777);
    int fd = open("/app/run/host.pid", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        std::string s = std::to_string(pid);
        write(fd, s.c_str(), s.size());
        close(fd);
    }
}
