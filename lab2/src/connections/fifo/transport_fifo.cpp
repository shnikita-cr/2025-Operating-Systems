#include "transport/transport_factory.h"
#include "transport/handshake_signal.h"
#include "transport/sync_semaphore.h"
#include "common/cli.h"
#include "common/common.h"
#include "conn_fifo.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>

static void unlinkQuiet(const std::string &p) {
    ::unlink(p.c_str());
}

enum FifoMsgType : unsigned char {
    FIFO_HELLO = 1,
    FIFO_TURN_REQ = 2,
    FIFO_TURN_RESP = 3,
    FIFO_RESULT = 4
};

static void pushU32(std::vector<unsigned char> &b, unsigned int v) {
    b.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>(v & 0xFF));
}

static void pushU16(std::vector<unsigned char> &b, unsigned int v) {
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    b.push_back(static_cast<unsigned char>(v & 0xFF));
}

static bool readU32(const unsigned char *p, std::size_t n, std::size_t &off, unsigned int &out) {
    if (off + 4 > n) return false;
    out = (static_cast<unsigned int>(p[off]) << 24)
          | (static_cast<unsigned int>(p[off + 1]) << 16)
          | (static_cast<unsigned int>(p[off + 2]) << 8)
          | (static_cast<unsigned int>(p[off + 3]));
    off += 4;
    return true;
}

static bool readU16(const unsigned char *p, std::size_t n, std::size_t &off, unsigned int &out) {
    if (off + 2 > n) return false;
    out = (static_cast<unsigned int>(p[off]) << 8)
          | (static_cast<unsigned int>(p[off + 1]));
    off += 2;
    return true;
}

static bool sendFrame(ConnIface &c, unsigned char type, const std::vector<unsigned char> &payload, int timeoutMs) {
    unsigned char hdr[9];
    hdr[0] = 'L';
    hdr[1] = 'B';
    hdr[2] = '2';
    hdr[3] = '!';
    hdr[4] = type;

    const unsigned int len = static_cast<unsigned int>(payload.size());
    hdr[5] = static_cast<unsigned char>((len >> 24) & 0xFF);
    hdr[6] = static_cast<unsigned char>((len >> 16) & 0xFF);
    hdr[7] = static_cast<unsigned char>((len >> 8) & 0xFF);
    hdr[8] = static_cast<unsigned char>(len & 0xFF);

    if (!c.writeExact(hdr, sizeof(hdr), timeoutMs)) return false;
    if (!payload.empty()) {
        if (!c.writeExact(payload.data(), payload.size(), timeoutMs)) return false;
    }
    return true;
}

static bool
recvFrame(ConnIface &c, unsigned char &outType, std::vector<unsigned char> &outPayload, int timeoutMs) {
    unsigned char hdr[9];
    if (!c.readExact(hdr, sizeof(hdr), timeoutMs)) return false;

    if (!(hdr[0] == 'L' && hdr[1] == 'B' && hdr[2] == '2' && hdr[3] == '!')) return false;
    outType = hdr[4];

    const unsigned int len = (static_cast<unsigned int>(hdr[5]) << 24)
                             | (static_cast<unsigned int>(hdr[6]) << 16)
                             | (static_cast<unsigned int>(hdr[7]) << 8)
                             | (static_cast<unsigned int>(hdr[8]));

    outPayload.assign(len, 0);
    if (len > 0) {
        if (!c.readExact(outPayload.data(), len, timeoutMs)) return false;
    }
    return true;
}

static int clampTimeout(int ms) {
    if (ms <= 0) return 5000;
    return (ms > 5000) ? 5000 : ms;
}

static std::string semH2C(int hostPid, int clientId) {
    return makeSemName("h2c", hostPid, clientId, "fifo");
}

static std::string semC2H(int host_pid, int clientId) {
    return makeSemName("c2h", host_pid, clientId, "fifo");
}

static bool openSemRetry(const std::string &name, int timeout_ms, NamedSemaphore &out, std::string &err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        std::string e;
        NamedSemaphore s = NamedSemaphore::open(name, false, &e);
        if (s.isValid()) {
            out = std::move(s);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            err = "Failed to open semaphore '" + name + "': " + e;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

class FifoClientLink final : public IClientLink {
public:
    FifoClientLink(int hostPid,
                   int clientId,
                   const std::string &nick,
                   std::unique_ptr<ConnFifo> conn,
                   NamedSemaphore semH2C,
                   NamedSemaphore semC2H,
                   const std::string &pathC2H,
                   const std::string &pathH2C,
                   bool ownerUnlink,
                   std::function<void(int)> onDisconnect)
            : host_pid_(hostPid),
              client_id_(clientId),
              nick_(nick),
              conn_(std::move(conn)),
              sem_h2c_(std::move(semH2C)),
              sem_c2h_(std::move(semC2H)),
              path_c2h_(pathC2H),
              path_h2c_(pathH2C),
              owner_unlink_(ownerUnlink),
              on_disconnect_(std::move(onDisconnect)) {}

    ~FifoClientLink() override { disconnect(); }

    int clientId() const override { return client_id_; }

    std::string nick() const override { return nick_; }

    bool requestTurn(const TurnRequest &req, int timeoutMs, TurnResponse &out) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive_) {
            last_error_ = "requestTurn on disconnected";
            return false;
        }

        const int tmo = clampTimeout(timeoutMs);

        std::vector<unsigned char> p;
        pushU32(p, static_cast<unsigned int>(req.roundIndex));
        p.push_back(static_cast<unsigned char>(req.currentStatus == KidStatus::ALIVE ? 1 : 2));

        if (!sendFrame(*conn_, FIFO_TURN_REQ, p, tmo)) {
            last_error_ = "send TURN_REQ failed: " + conn_->lastError();
            return false;
        }

        std::string e;
        if (!sem_h2c_.post(&e)) {
            last_error_ = "sem_h2c post failed: " + e;
            return false;
        }

        e.clear();
        if (!sem_c2h_.waitMs(tmo, &e)) {
            last_error_ = "sem_c2h wait failed: " + e;
            return false;
        }

        unsigned char type = 0;
        std::vector<unsigned char> pl;
        if (!recvFrame(*conn_, type, pl, tmo)) {
            last_error_ = "recv TURN_RESP failed: " + conn_->lastError();
            return false;
        }
        if (type != FIFO_TURN_RESP) {
            last_error_ = "unexpected msg type for TURN_RESP";
            return false;
        }

        std::size_t off = 0;
        unsigned int num = 0;
        if (!readU32(pl.data(), pl.size(), off, num)) {
            last_error_ = "bad TURN_RESP payload";
            return false;
        }
        out.number = static_cast<int>(num);
        return true;
    }

    bool sendResult(const ResultMessage &msg, int timeout_ms) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive_) {
            last_error_ = "sendResult on disconnected";
            return false;
        }

        const int tmo = clampTimeout(timeout_ms);

        std::vector<unsigned char> p;
        pushU32(p, static_cast<unsigned int>(msg.roundIndex));
        pushU32(p, static_cast<unsigned int>(msg.wolfNumber));
        pushU32(p, static_cast<unsigned int>(msg.kidNumber));
        p.push_back(static_cast<unsigned char>(msg.hid ? 1 : 0));
        p.push_back(static_cast<unsigned char>(msg.resurrected ? 1 : 0));
        p.push_back(static_cast<unsigned char>(msg.newStatus == KidStatus::ALIVE ? 1 : 2));
        p.push_back(static_cast<unsigned char>(msg.gameOver ? 1 : 0));

        if (!sendFrame(*conn_, FIFO_RESULT, p, tmo)) {
            last_error_ = "send RESULT failed: " + conn_->lastError();
            return false;
        }

        std::string e;
        if (!sem_h2c_.post(&e)) {
            last_error_ = "sem_h2c post failed: " + e;
            return false;
        }
        return true;
    }

    std::string lastError() const override { return last_error_; }

    void disconnect() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive_) return;
        alive_ = false;

        if (conn_) conn_->close();
        sem_h2c_.close();
        sem_c2h_.close();

        if (owner_unlink_) {
            if (!path_c2h_.empty()) unlinkQuiet(path_c2h_);
            if (!path_h2c_.empty()) unlinkQuiet(path_h2c_);
        }

        if (on_disconnect_) on_disconnect_(client_id_);
    }

private:
    int host_pid_ = 0;
    int client_id_ = 0;
    std::string nick_;

    std::unique_ptr<ConnFifo> conn_;
    NamedSemaphore sem_h2c_;
    NamedSemaphore sem_c2h_;

    std::string path_c2h_;
    std::string path_h2c_;
    bool owner_unlink_ = false;

    std::function<void(int)> on_disconnect_;

    mutable std::mutex mu_;
    bool alive_ = true;
    std::string last_error_;
};

class FifoHostSession final : public IHostSession {
public:
    FifoHostSession(const HostArgs &args, const std::shared_ptr<Logger> &log)
            : args_(args), log_(log), host_pid_(getPid()) {}

    ~FifoHostSession() override { shutdown(); }

    void startAccepting() override {
        std::lock_guard<std::mutex> lk(start_mu_);
        if (started_) return;
        started_ = true;

        std::string e;
        if (!hs_.init(&e)) {
            last_error_ = "Handshake init failed: " + e;
            if (log_) log_->error(last_error_);
            return;
        }

        stop_.store(false);
        th_handshake_ = std::thread([this] { handshakeLoop(); });

        if (log_) log_->info("FifoHostSession started. Waiting for clients via SIGUSR1...");
    }

    std::vector<std::shared_ptr<IClientLink>> clientsSnapshot() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::shared_ptr<IClientLink>> v;
        v.reserve(clients_.size());
        for (auto &kv: clients_) v.push_back(kv.second);
        return v;
    }

    std::string lastError() const override { return last_error_; }

    void shutdown() override {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true)) {

        }

        {
            std::lock_guard<std::mutex> lk(start_mu_);
            if (!started_) return;
        }

        if (th_handshake_.joinable()) th_handshake_.join();

        std::unordered_map<int, std::shared_ptr<FifoClientLink>> local;
        {
            std::lock_guard<std::mutex> lk(mu_);
            local.swap(clients_);
        }

        for (auto &kv: local) kv.second->disconnect();

        if (log_) log_->info("FifoHostSession shutdown complete");
    }

private:
    void handshakeLoop() {
        while (!stop_.load()) {
            HandshakeEvent ev;
            std::string e;
            if (!hs_.waitNextClient(5000, ev, &e)) {
                if (log_) log_->info("Handshake wait: " + e);
                continue;
            }

            if (stop_.load()) break;


            std::string path_c2h, path_h2c;
            ConnFifo conn;
            if (!ConnFifo::hostCreateAndOpen(host_pid_, ev.clientId, conn, path_c2h, path_h2c, e)) {
                if (log_) log_->error("FIFO create/open failed: " + e);
                continue;
            }

            const std::string sem_h2c_name = semH2C(host_pid_, ev.clientId);
            const std::string sem_c2h_name = semC2H(host_pid_, ev.clientId);

            std::string se;
            NamedSemaphore sem_h2c = NamedSemaphore::create(sem_h2c_name, 0, true, &se);
            if (!sem_h2c.isValid()) {
                if (log_) log_->error("Failed to create sem_h2c: " + se);
                conn.close();
                unlinkQuiet(path_c2h);
                unlinkQuiet(path_h2c);
                continue;
            }

            se.clear();
            NamedSemaphore sem_c2h = NamedSemaphore::create(sem_c2h_name, 0, true, &se);
            if (!sem_c2h.isValid()) {
                if (log_) log_->error("Failed to create sem_c2h: " + se);
                conn.close();
                sem_h2c.close();
                unlinkQuiet(path_c2h);
                unlinkQuiet(path_h2c);
                continue;
            }


            std::unique_ptr<ConnFifo> connp(new ConnFifo(std::move(conn)));

            unsigned char type = 0;
            std::vector<unsigned char> pl;
            if (!recvFrame(*connp, type, pl, clampTimeout(args_.timeoutMs))) {
                if (log_)
                    log_->error("HELLO recv failed for clientId=" + std::to_string(ev.clientId) +
                                ": " + connp->lastError());
                connp->close();
                sem_h2c.close();
                sem_c2h.close();
                unlinkQuiet(path_c2h);
                unlinkQuiet(path_h2c);
                continue;
            }
            if (type != FIFO_HELLO) {
                if (log_) log_->error("First msg is not HELLO for clientId=" + std::to_string(ev.clientId));
                connp->close();
                sem_h2c.close();
                sem_c2h.close();
                unlinkQuiet(path_c2h);
                unlinkQuiet(path_h2c);
                continue;
            }

            std::size_t off = 0;
            unsigned int pid_u = 0;
            unsigned int cid_u = 0;
            unsigned int nick_len = 0;
            if (!readU32(pl.data(), pl.size(), off, pid_u)) { goto bad_hello; }
            if (!readU32(pl.data(), pl.size(), off, cid_u)) { goto bad_hello; }
            if (!readU16(pl.data(), pl.size(), off, nick_len)) { goto bad_hello; }
            if (off + nick_len > pl.size()) { goto bad_hello; }

            if (static_cast<int>(pid_u) != ev.clientPid || static_cast<int>(cid_u) != ev.clientId) {
                if (log_)
                    log_->error("HELLO mismatch: expected pid=" + std::to_string(ev.clientPid) +
                                " cid=" + std::to_string(ev.clientId) +
                                " got pid=" + std::to_string(pid_u) +
                                " cid=" + std::to_string(cid_u));
                goto bad_hello;
            }

            {
                std::string nick(reinterpret_cast<const char *>(pl.data() + off),
                                 reinterpret_cast<const char *>(pl.data() + off + nick_len));

                auto on_disc = [this](int cid) { removeClient(cid); };

                std::shared_ptr<FifoClientLink> link(new FifoClientLink(
                        host_pid_, ev.clientId, nick, std::move(connp),
                        std::move(sem_h2c), std::move(sem_c2h),
                        path_c2h, path_h2c, true, on_disc));

                {
                    std::lock_guard<std::mutex> lk(mu_);
                    clients_[ev.clientId] = link;
                }

                if (log_) {
                    log_->info("Client connected (fifo): id=" + std::to_string(ev.clientId) +
                               " pid=" + std::to_string(ev.clientPid) +
                               " nick=" + nick +
                               " fifo_c2h=" + path_c2h +
                               " fifo_h2c=" + path_h2c);
                }
            }

            continue;

            bad_hello:
            if (log_) log_->error("Bad HELLO payload for clientId=" + std::to_string(ev.clientId));
            connp->close();
            sem_h2c.close();
            sem_c2h.close();
            unlinkQuiet(path_c2h);
            unlinkQuiet(path_h2c);
        }
    }

    void removeClient(int clientId) {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(clientId);
    }

private:
    HostArgs args_;
    std::shared_ptr<Logger> log_;
    const int host_pid_;

    mutable std::mutex mu_;
    std::unordered_map<int, std::shared_ptr<FifoClientLink>> clients_;

    std::mutex start_mu_;
    bool started_ = false;
    std::atomic<bool> stop_{false};

    HandshakeHost hs_;
    std::thread th_handshake_;

    std::string last_error_;
};

class FifoClientSession final : public IClientSession {
public:
    FifoClientSession(const ClientArgs &args, const std::shared_ptr<Logger> &log)
            : args_(args), log_(log) {}

    ~FifoClientSession() override { shutdown(); }

    bool connect() override {
        const int tmo = clampTimeout(args_.timeoutMs);

        std::string e;
        int client_id = 0;
        if (!handshakeClient(args_.hostPid, tmo, client_id, &e)) {
            last_error_ = "Handshake client failed: " + e;
            if (log_) log_->error(last_error_);
            return false;
        }
        client_id_ = client_id;


        ConnFifo conn;
        std::string path_c2h, path_h2c;
        if (!ConnFifo::clientOpenWithRetry(args_.hostPid, client_id_, tmo, conn, path_c2h, path_h2c, e)) {
            last_error_ = "FIFO open failed: " + e;
            if (log_) log_->error(last_error_);
            return false;
        }
        conn_.reset(new ConnFifo(std::move(conn)));


        const std::string sem_h2c_name = semH2C(args_.hostPid, client_id_);
        const std::string sem_c2h_name = semC2H(args_.hostPid, client_id_);

        if (!openSemRetry(sem_h2c_name, tmo, sem_h2c_, e)) {
            last_error_ = e;
            if (log_) log_->error(last_error_);
            return false;
        }
        if (!openSemRetry(sem_c2h_name, tmo, sem_c2h_, e)) {
            last_error_ = e;
            if (log_) log_->error(last_error_);
            return false;
        }


        std::vector<unsigned char> p;
        pushU32(p, static_cast<unsigned int>(getPid()));
        pushU32(p, static_cast<unsigned int>(client_id_));
        pushU16(p, static_cast<unsigned int>(args_.nick.size()));
        for (char ch: args_.nick) p.push_back(static_cast<unsigned char>(ch));

        if (!sendFrame(*conn_, FIFO_HELLO, p, tmo)) {
            last_error_ = "send HELLO failed: " + conn_->lastError();
            if (log_) log_->error(last_error_);
            return false;
        }

        if (log_) {
            log_->info("Connected (fifo). clientId=" + std::to_string(client_id_) +
                       " fifo_c2h=" + path_c2h +
                       " fifo_h2c=" + path_h2c);
        }

        connected_ = true;
        return true;
    }

    bool waitTurnRequest(int timeout_ms, TurnRequest &out_req) override {
        if (!connected_) {
            last_error_ = "waitTurnRequest: not connected";
            return false;
        }

        const int tmo = clampTimeout(timeout_ms);
        std::string e;
        if (!sem_h2c_.waitMs(tmo, &e)) {
            last_error_ = "sem_h2c wait failed: " + e;
            return false;
        }

        unsigned char type = 0;
        std::vector<unsigned char> pl;
        if (!recvFrame(*conn_, type, pl, tmo)) {
            last_error_ = "recv TURN_REQ failed: " + conn_->lastError();
            return false;
        }
        if (type != FIFO_TURN_REQ) {
            last_error_ = "unexpected message type (expected TURN_REQ)";
            return false;
        }

        std::size_t off = 0;
        unsigned int round_u = 0;
        if (!readU32(pl.data(), pl.size(), off, round_u)) {
            last_error_ = "bad TURN_REQ payload";
            return false;
        }
        if (off + 1 > pl.size()) {
            last_error_ = "bad TURN_REQ payload size";
            return false;
        }
        const unsigned char st = pl[off];

        out_req.roundIndex = static_cast<int>(round_u);
        out_req.currentStatus = (st == 1) ? KidStatus::ALIVE : KidStatus::DEAD;
        return true;
    }

    bool sendTurn(const TurnResponse &resp, int timeout_ms) override {
        if (!connected_) {
            last_error_ = "sendTurn: not connected";
            return false;
        }

        const int tmo = clampTimeout(timeout_ms);
        std::vector<unsigned char> p;
        pushU32(p, static_cast<unsigned int>(resp.number));

        if (!sendFrame(*conn_, FIFO_TURN_RESP, p, tmo)) {
            last_error_ = "send TURN_RESP failed: " + conn_->lastError();
            return false;
        }

        std::string e;
        if (!sem_c2h_.post(&e)) {
            last_error_ = "sem_c2h post failed: " + e;
            return false;
        }

        return true;
    }

    bool waitResult(int timeout_ms, ResultMessage &out_msg) override {
        if (!connected_) {
            last_error_ = "waitResult: not connected";
            return false;
        }

        const int tmo = clampTimeout(timeout_ms);
        std::string e;
        if (!sem_h2c_.waitMs(tmo, &e)) {
            last_error_ = "sem_h2c wait failed: " + e;
            return false;
        }

        unsigned char type = 0;
        std::vector<unsigned char> pl;
        if (!recvFrame(*conn_, type, pl, tmo)) {
            last_error_ = "recv RESULT failed: " + conn_->lastError();
            return false;
        }
        if (type != FIFO_RESULT) {
            last_error_ = "unexpected message type (expected RESULT)";
            return false;
        }

        std::size_t off = 0;
        unsigned int round_u = 0, wolf_u = 0, kid_u = 0;
        if (!readU32(pl.data(), pl.size(), off, round_u)) {
            last_error_ = "bad RESULT payload";
            return false;
        }
        if (!readU32(pl.data(), pl.size(), off, wolf_u)) {
            last_error_ = "bad RESULT payload";
            return false;
        }
        if (!readU32(pl.data(), pl.size(), off, kid_u)) {
            last_error_ = "bad RESULT payload";
            return false;
        }
        if (off + 4 > pl.size()) {
            last_error_ = "bad RESULT payload size";
            return false;
        }

        const unsigned char hid = pl[off++];
        const unsigned char res = pl[off++];
        const unsigned char st = pl[off++];
        const unsigned char go = pl[off++];

        out_msg.roundIndex = static_cast<int>(round_u);
        out_msg.wolfNumber = static_cast<int>(wolf_u);
        out_msg.kidNumber = static_cast<int>(kid_u);
        out_msg.hid = (hid != 0);
        out_msg.resurrected = (res != 0);
        out_msg.newStatus = (st == 1) ? KidStatus::ALIVE : KidStatus::DEAD;
        out_msg.gameOver = (go != 0);

        return true;
    }

    std::string lastError() const override { return last_error_; }

    void shutdown() override {
        if (!connected_) return;
        connected_ = false;

        if (conn_) conn_->close();
        sem_h2c_.close();
        sem_c2h_.close();
    }

private:
    ClientArgs args_;
    std::shared_ptr<Logger> log_;

    bool connected_ = false;
    int client_id_ = 0;

    std::unique_ptr<ConnFifo> conn_;
    NamedSemaphore sem_h2c_;
    NamedSemaphore sem_c2h_;

    std::string last_error_;
};

HostRuntime createHostRuntime(int argc, char **argv) {
    HostRuntime rt;
    rt.transportCode = "fifo";

    std::string err;
    if (!parseHostArgs(argc, argv, rt.args, &err)) {
        std::fprintf(stderr, "host_fifo args error: %s\n", err.c_str());
        return rt;
    }

    rt.log = std::make_shared<Logger>(rt.args.logDir, rt.transportCode, "host", rt.args.nick, "host.log");
    rt.session = std::unique_ptr<IHostSession>(new FifoHostSession(rt.args, rt.log));
    return rt;
}

ClientRuntime createClientRuntime(int argc, char **argv) {
    ClientRuntime rt;
    rt.transportCode = "fifo";

    std::string err;
    if (!parseClientArgs(argc, argv, rt.args, &err)) {
        std::fprintf(stderr, "client_fifo args error: %s\n", err.c_str());
        return rt;
    }

    const int pid = getPid();
    const std::string fname = "client_" + std::to_string(pid) + ".log";
    rt.log = std::make_shared<Logger>(rt.args.logDir, rt.transportCode, "client", rt.args.nick, fname);
    rt.session = std::unique_ptr<IClientSession>(new FifoClientSession(rt.args, rt.log));
    return rt;
}
