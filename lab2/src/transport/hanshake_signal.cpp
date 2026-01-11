#include "handshake_signal.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <time.h>
#include <pthread.h>

static void makeAbsDeadline(int timeout_ms, struct timespec &out_ts) {
    out_ts.tv_sec = timeout_ms / 1000;
    out_ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
}

HandshakeHost::HandshakeHost() = default;

bool HandshakeHost::init(std::string *err) {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGUSR1);
    ::sigaddset(&set, SIGUSR2);

    if (::pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
        if (err) *err = "pthread_sigmask(SIG_BLOCK) failed";
        return false;
    }

    inited_ = true;
    return true;
}

bool HandshakeHost::waitNextClient(int timeoutMs, HandshakeEvent &out_ev, std::string *err) {
    if (!inited_) {
        if (err) *err = "HandshakeHost is not initialized";
        return false;
    }

    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGUSR1);

    siginfo_t si;
    std::memset(&si, 0, sizeof(si));

    struct timespec to;
    makeAbsDeadline(timeoutMs, to);

    const int signo = ::sigtimedwait(&set, &si, &to);
    if (signo < 0) {
        if (errno == EAGAIN) {
            if (err) *err = "Handshake timeout waiting SIGUSR1";
            return false;
        }
        if (errno == EINTR) {
            if (err) *err = "Handshake interrupted waiting SIGUSR1";
            return false;
        }
        if (err) {
            std::ostringstream oss;
            oss << "sigtimedwait(SIGUSR1) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }

    if (signo != SIGUSR1) {
        if (err) *err = "Unexpected signal in handshake";
        return false;
    }

    const int client_pid = static_cast<int>(si.si_pid);
    const int client_id = next_id_++;

    union sigval sv;
    sv.sival_int = client_id;
    if (::sigqueue(client_pid, SIGUSR2, sv) != 0) {
        if (err) {
            std::ostringstream oss;
            oss << "sigqueue(SIGUSR2) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }

    out_ev.clientPid = client_pid;
    out_ev.clientId = client_id;
    return true;
}

bool handshakeClient(int hostPid, int timeoutMs, int &outClientId, std::string *err) {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGUSR2);

    if (::pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
        if (err) *err = "pthread_sigmask(SIG_BLOCK, SIGUSR2) failed";
        return false;
    }

    if (::kill(hostPid, SIGUSR1) != 0) {
        if (err) {
            std::ostringstream oss;
            oss << "kill(SIGUSR1) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }

    siginfo_t si;
    std::memset(&si, 0, sizeof(si));

    struct timespec to;
    makeAbsDeadline(timeoutMs, to);

    const int signo = ::sigtimedwait(&set, &si, &to);
    if (signo < 0) {
        if (errno == EAGAIN) {
            if (err) *err = "Handshake timeout waiting SIGUSR2";
            return false;
        }
        if (errno == EINTR) {
            if (err) *err = "Handshake interrupted waiting SIGUSR2";
            return false;
        }
        if (err) {
            std::ostringstream oss;
            oss << "sigtimedwait(SIGUSR2) failed: errno=" << errno << " (" << std::strerror(errno) << ")";
            *err = oss.str();
        }
        return false;
    }

    if (signo != SIGUSR2) {
        if (err) *err = "Unexpected signal in handshake client";
        return false;
    }

    outClientId = si.si_value.sival_int;
    if (outClientId <= 0) {
        if (err) *err = "Invalid clientId received in SIGUSR2";
        return false;
    }

    return true;
}
