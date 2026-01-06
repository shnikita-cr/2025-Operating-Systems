#include "common/common.h"
#include "connections/base/conn_iface.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore.h>
#include <signal.h>
#include <string>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector> 

#ifdef TRANSPORT_MQ
#include <mqueue.h>
#endif 

#ifdef TRANSPORT_SOCK
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif 

#ifdef TRANSPORT_MQ
static const char* kMode = "mq";
#elif defined(TRANSPORT_FIFO)
static const char *kMode = "fifo";
#elif defined(TRANSPORT_SOCK)
static const char* kMode = "sock";
#else
#error "Transport macro not defined"
#endif 

struct ClientCtx {
    uint32_t id = 0;
    pid_t pid = 0;
    uint64_t created_at_sec = 0; 

    std::atomic<bool> connected{false};
    bool alive = true; 

    std::unique_ptr<IConn> conn;
    sem_t *step_sem = nullptr;
    std::string nick; 
 

    std::thread worker;
    bool worker_started = false; 

    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool want_turn = false;
    uint32_t want_round = 0;
    bool turn_ready = false;
    bool turn_ok = false;
    int turn_value = -1;
}; 

static std::mutex g_mu;
static std::map<uint32_t, std::shared_ptr<ClientCtx>> g_clients;
static std::atomic<uint32_t> g_next_id{1};
static std::atomic<bool> g_stop{false}; 

static Logger *g_log = nullptr;
static pid_t g_host_pid = 0; 

static void send_sigusr2_with_id(pid_t cpid, uint32_t id) {
    union sigval sv;
    sv.sival_int = (int) id;
    sigqueue(cpid, SIGUSR2, sv);
} 

static std::string safe_nick(const char *s, size_t n) {
    if (!s || n == 0) return {};
    size_t len = 0;
    for (; len < n && s[len] != 0; ++len) {}
    return std::string(s, len);
} 

static void stop_worker(const std::shared_ptr<ClientCtx> &c) {
    if (!c) return;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        c->stop = true;
        c->cv.notify_all();
    }
    if (c->worker.joinable()) c->worker.join();
} 

static void cleanup_ipc_for_client(uint32_t cid) { 

    std::string sname = sem_name_for(g_host_pid, cid);
    sem_unlink(sname.c_str()); 

#ifdef TRANSPORT_MQ
    mq_unlink(mq_name_in(g_host_pid, cid).c_str());
    mq_unlink(mq_name_out(g_host_pid, cid).c_str());
#elif defined(TRANSPORT_FIFO)
    ::unlink(fifo_name_in(g_host_pid, cid).c_str());
    ::unlink(fifo_name_out(g_host_pid, cid).c_str());
#endif
} 

static void remove_client(uint32_t cid, const std::string &reason) {
    std::shared_ptr<ClientCtx> c;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_clients.find(cid);
        if (it == g_clients.end()) return;
        c = it->second;
        g_clients.erase(it);
    } 

    if (g_log) g_log->err("Disconnect c" + std::to_string(cid) + ": " + reason); 

    stop_worker(c); 

    if (c->step_sem) {
        sem_close(c->step_sem);
        c->step_sem = nullptr;
    }
    c->conn.reset(); 

    cleanup_ipc_for_client(cid);
} 

static void start_worker_if_needed(const std::shared_ptr<ClientCtx> &c, bool need_hello, int timeout_ms) {
    if (!c) return;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        if (c->worker_started) return;
        c->worker_started = true;
    } 

    c->worker = std::thread([c, need_hello, timeout_ms]() { 

        if (need_hello) {
            Msg hello{};
            if (!c->conn || !c->conn->recv(&hello, sizeof(hello), timeout_ms) || hello.type != HELLO) {
                if (g_log) g_log->err("Handshake HELLO failed for c" + std::to_string(c->id));
                c->connected.store(false);
                return;
            }
            c->nick = safe_nick(hello.nick, sizeof(hello.nick)); 

            Msg ack{};
            ack.type = HELLO_ACK;
            ack.client_id = c->id;
            strncpy(ack.nick, "host", sizeof(ack.nick) - 1);
            if (!c->conn->send(&ack, sizeof(ack), timeout_ms)) {
                if (g_log) g_log->err("Handshake HELLO_ACK send failed for c" + std::to_string(c->id));
                c->connected.store(false);
                return;
            }
            c->connected.store(true);
            if (g_log) g_log->log("Client connected id=" + std::to_string(c->id) + " nick=" + c->nick);
        } 

        std::unique_lock<std::mutex> lk(c->mu);
        while (!c->stop) {
            c->cv.wait(lk, [&]() { return c->stop || c->want_turn; });
            if (c->stop) break; 

            const uint32_t want_round = c->want_round;
            lk.unlock(); 

            Msg t{};
            bool ok = false;
            int v = -1; 

            if (c->conn && c->conn->recv(&t, sizeof(t), timeout_ms)) {
                ok = (t.type == TURN) && (t.client_id == c->id) && (t.round_no == want_round) && (t.value >= 1) &&
                     (t.value <= 100);
                if (ok) v = (int) t.value;
            } 

            lk.lock();
            c->turn_ready = true;
            c->turn_ok = ok;
            c->turn_value = v;
            c->want_turn = false;
            c->cv.notify_all();
        }
    });
} 

static void arm_turn(const std::shared_ptr<ClientCtx> &c, uint32_t round_no) {
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mu);
    if (!c->connected.load() || !c->conn) return; 

    c->want_round = round_no;
    c->want_turn = true;
    c->turn_ready = false;
    c->turn_ok = false;
    c->turn_value = -1;
    c->cv.notify_all();
} 

static bool try_take_turn(const std::shared_ptr<ClientCtx> &c, int &out_val, bool &out_ok) {
    if (!c) return false;
    std::lock_guard<std::mutex> lk(c->mu);
    if (!c->turn_ready) return false; 

    out_ok = c->turn_ok;
    out_val = c->turn_value; 
  

    c->turn_ready = false;
    c->turn_ok = false;
    c->turn_value = -1;  

    return true;
}  

static std::vector<std::shared_ptr<ClientCtx>> snapshot_connected_clients() {
    std::vector<std::shared_ptr<ClientCtx>> out;
    std::lock_guard<std::mutex> lk(g_mu);
    out.reserve(g_clients.size());
    for (auto &kv: g_clients) {
        if (kv.second && kv.second->connected.load() && kv.second->conn) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const std::shared_ptr<ClientCtx> &a, const std::shared_ptr<ClientCtx> &b) {
        return a->id < b->id;
    });
    return out;
}  

static void cleanup_stale_unconnected(int timeout_ms) {
    const uint64_t now = epoch_sec();
    const uint64_t ttl = (uint64_t) (timeout_ms / 1000) + 2;  

    std::vector<uint32_t> to_drop;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &kv: g_clients) {
            auto &c = kv.second;
            if (!c) continue;
            if (c->connected.load()) continue;
            if (c->created_at_sec != 0 && (now - c->created_at_sec) >= ttl) {
                to_drop.push_back(kv.first);
            }
        }
    } 

    for (uint32_t cid: to_drop) {
        remove_client(cid, "HELLO/connect timeout");
    }
} 

#ifdef TRANSPORT_SOCK
static void accept_thread_sock(int listen_fd, int timeout_ms, const std::string& addr, int port) {
    if (g_log) g_log->log("Listening on " + addr + ":" + std::to_string(port)); 

    while (!g_stop.load()) {
        struct pollfd pfd{listen_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue; 

        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cfd = accept(listen_fd, (struct sockaddr*)&sa, &sl);
        if (cfd < 0) continue; 

        std::unique_ptr<IConn> conn(create_conn_sock_host_from_fd(cfd));
        if (!conn) {
            close(cfd);
            continue;
        } 

        Msg hello{};
        if (!conn->recv(&hello, sizeof(hello), timeout_ms) || hello.type != HELLO) {
             
            continue;
        } 

        const uint32_t cid = hello.client_id;
        std::shared_ptr<ClientCtx> c;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_clients.find(cid);
            if (it != g_clients.end()) c = it->second;
        }
        if (!c) {
             
            continue;
        } 

        c->nick = safe_nick(hello.nick, sizeof(hello.nick));
        c->conn = std::move(conn); 

        Msg ack{};
        ack.type = HELLO_ACK;
        ack.client_id = cid;
        strncpy(ack.nick, "host", sizeof(ack.nick) - 1);
        if (!c->conn->send(&ack, sizeof(ack), timeout_ms)) {
            remove_client(cid, "HELLO_ACK send failed");
            continue;
        } 

        c->connected.store(true);
        start_worker_if_needed(c, false, timeout_ms); 

        if (g_log) g_log->log("Client connected via sock id=" + std::to_string(cid) + " nick=" + c->nick);
    } 

    close(listen_fd);
}
#endif 

static void handle_new_clients(const CliArgsHost &args) {
    pid_t cpid;
    while (pop_pending_client(cpid)) {
        uint32_t id = g_next_id.fetch_add(1); 

        auto c = std::make_shared<ClientCtx>();
        c->id = id;
        c->pid = cpid;
        c->created_at_sec = epoch_sec();
        c->alive = true;
        c->connected.store(false); 
 

        std::string sname = sem_name_for(g_host_pid, id);
        sem_unlink(sname.c_str());
        c->step_sem = sem_open(sname.c_str(), O_CREAT, 0666, 0);
        if (!c->step_sem) {
            if (g_log) g_log->err("sem_open failed for c" + std::to_string(id));
            cleanup_ipc_for_client(id);
            continue;
        } 

#ifdef TRANSPORT_MQ
        c->conn.reset(create_conn_mq_host(g_host_pid, id));
        if (!c->conn) {
            sem_close(c->step_sem);
            c->step_sem = nullptr;
            cleanup_ipc_for_client(id);
            if (g_log) g_log->err("mq init failed for c" + std::to_string(id));
            continue;
        }
#elif defined(TRANSPORT_FIFO)
        c->conn.reset(create_conn_fifo_host(g_host_pid, id));
        if (!c->conn) {
            sem_close(c->step_sem);
            c->step_sem = nullptr;
            cleanup_ipc_for_client(id);
            if (g_log) g_log->err("fifo init failed for c" + std::to_string(id));
            continue;
        }
#elif defined(TRANSPORT_SOCK) 

#endif 

        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_clients[id] = c;
        } 
 

        send_sigusr2_with_id(cpid, id);
        if (g_log) g_log->log("Handshake: pid=" + std::to_string(cpid) + " -> id=" + std::to_string(id)); 

#if defined(TRANSPORT_MQ) || defined(TRANSPORT_FIFO) 

        start_worker_if_needed(c, true, args.timeout_ms);
#endif
    }
} 

static bool send_msg(const std::shared_ptr<ClientCtx> &c, const Msg &m, int timeout_ms) {
    if (!c || !c->conn || !c->connected.load()) return false;
    return c->conn->send(&m, sizeof(m), timeout_ms);
} 

int main(int argc, char **argv) {
    CliArgsHost args;
    parse_host_args(argc, argv, args); 

    ensure_dir(args.log_dir);
    std::string typed_log_dir = args.log_dir;
    if (!typed_log_dir.empty() && typed_log_dir.back() != '/') typed_log_dir += "/";
    typed_log_dir += kMode;
    ensure_dir(typed_log_dir); 

    Logger log(typed_log_dir + "/host.log", "host", kMode);
    g_log = &log;
    install_sigusr1_host();
    rng_seed(); 

    g_host_pid = getpid();
    write_host_pid_file(g_host_pid); 

    log.log(std::string("Host start pid=") + std::to_string(g_host_pid) + " mode=" + kMode); 

#ifdef TRANSPORT_SOCK
    int listen_fd = sock_listen_create(args.addr.c_str(), args.port);
    if (listen_fd < 0) {
        log.err("socket listen failed");
        return 1;
    }
    std::thread accept_thr(accept_thread_sock, listen_fd, args.timeout_ms, args.addr, args.port);
    accept_thr.detach();
#endif 

    int rounds_all_dead_in_row = 0;
    int round_no = 0; 

    while (!g_stop.load()) {
        handle_new_clients(args);
        cleanup_stale_unconnected(args.timeout_ms); 

        auto clients = snapshot_connected_clients();
        if (clients.empty()) {
            usleep(200 * 1000);
            continue;
        } 

        ++round_no; 

        int wolf = try_read_number_3s();
        if (wolf < 1) wolf = rng_uniform(1, 100); 

        log.log("Round " + std::to_string(round_no) + ": wolf=" + std::to_string(wolf) + " clients=" +
                std::to_string((int) clients.size())); 
 

        for (auto &c: clients) {
            Msg rs{};
            rs.type = ROUND_START;
            rs.client_id = c->id;
            rs.round_no = (uint32_t) round_no;
            rs.value = (uint32_t) wolf;
            strncpy(rs.nick, "host", sizeof(rs.nick) - 1);
            if (!send_msg(c, rs, args.timeout_ms)) {
                remove_client(c->id, "ROUND_START send failed");
            }
        } 
 

        clients = snapshot_connected_clients();
        if (clients.empty()) {
            usleep(200 * 1000);
            continue;
        } 
 

        for (auto &c: clients) {
            if (!c->step_sem) {
                remove_client(c->id, "step semaphore missing");
                continue;
            }
            sem_post(c->step_sem);
        } 
 

        clients = snapshot_connected_clients();
        if (clients.empty()) {
            usleep(200 * 1000);
            continue;
        } 

        const double n = std::max(1.0, (double) clients.size()); 
 

        for (auto &c: clients) arm_turn(c, (uint32_t) round_no); 

        int alive_cnt = 0;
        int hidden_cnt = 0;
        int caught_cnt = 0;
        int revived_cnt = 0;
        int dead_cnt = 0; 

        std::string turns_log = "Turns:"; 
 

        std::map<uint32_t, bool> done;
        for (auto &c: clients) done[c->id] = false; 

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.timeout_ms); 
 

        while (std::chrono::steady_clock::now() < deadline) {
            bool any_progress = false; 

            for (auto &c: clients) {
                if (!c) continue;
                if (done[c->id]) continue; 

                int v = -1;
                bool ok = false;
                if (!try_take_turn(c, v, ok)) continue; 

                done[c->id] = true;
                any_progress = true; 

                if (!ok) {
                    remove_client(c->id, "TURN invalid");
                    continue;
                } 

                turns_log += " c" + std::to_string(c->id) + "=" + std::to_string(v); 

                const bool was_alive = c->alive;
                const int d = std::abs(v - wolf);
                const int hide_thr = (int) (70.0 / n);
                const int rez_thr = (int) (20.0 / n); 

                bool now_alive = false;
                std::string outcome;
                if (was_alive) {
                    bool hidden = (d <= hide_thr);
                    now_alive = hidden;
                    if (hidden) {
                        outcome = "hidden";
                        hidden_cnt++;
                    } else {
                        outcome = "caught";
                        caught_cnt++;
                    }
                } else {
                    bool revived = (d <= rez_thr);
                    now_alive = revived;
                    if (revived) {
                        outcome = "revived";
                        revived_cnt++;
                    } else {
                        outcome = "dead";
                    }
                } 

                c->alive = now_alive;
                if (now_alive) alive_cnt++;
                else dead_cnt++; 

                Msg r{};
                r.type = RESULT;
                r.client_id = c->id;
                r.round_no = (uint32_t) round_no;
                r.state = now_alive ? ST_ALIVE : ST_DEAD;
                strncpy(r.nick, "host", sizeof(r.nick) - 1);
                if (!send_msg(c, r, args.timeout_ms)) {
                    remove_client(c->id, "RESULT send failed");
                    continue;
                } 

                log.log(
                        "c" + std::to_string(c->id) + "(" + c->nick + ") pre=" + (was_alive ? "alive" : "dead") +
                        " turn=" + std::to_string(v) + " d=" + std::to_string(d) +
                        " thr_hide=" + std::to_string(hide_thr) + " thr_rez=" + std::to_string(rez_thr) +
                        " => " + outcome
                );
            } 
 

            bool all_done = true;
            for (auto &kv: done) {
                if (!kv.second) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break; 

            if (!any_progress) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } 
 

        for (auto &c: clients) {
            if (!c) continue;
            if (!done[c->id]) {
                remove_client(c->id, "TURN timeout");
            }
        } 
 

        int alive_now = 0;
        {
            auto still = snapshot_connected_clients();
            for (auto &c: still) {
                if (c && c->alive) alive_now++;
            }
        } 

        log.log(turns_log);
        log.log("Round summary: hidden=" + std::to_string(hidden_cnt) +
                " caught=" + std::to_string(caught_cnt) +
                " revived=" + std::to_string(revived_cnt) +
                " dead=" + std::to_string(dead_cnt) +
                " alive_now=" + std::to_string(alive_now)); 

        if (alive_now == 0) rounds_all_dead_in_row++;
        else rounds_all_dead_in_row = 0; 

        log.log("All-dead-in-row=" + std::to_string(rounds_all_dead_in_row)); 

        bool need_stop = false;
        if (rounds_all_dead_in_row >= 2) {
            log.log("Game over condition met (2 rounds all dead)");
            need_stop = true;
        }
        if (args.rounds > 0 && round_no >= args.rounds) {
            log.log("Max rounds reached");
            need_stop = true;
        } 

        if (need_stop) {
            auto final_clients = snapshot_connected_clients();
            for (auto &c: final_clients) {
                Msg go{};
                go.type = GAME_OVER;
                go.client_id = c->id;
                strncpy(go.nick, "host", sizeof(go.nick) - 1);
                send_msg(c, go, args.timeout_ms);
            }
            break;
        }
    } 

    g_stop.store(true); 
 

    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &kv: g_clients) ids.push_back(kv.first);
    }
    for (uint32_t cid: ids) remove_client(cid, "host shutdown"); 

#ifdef TRANSPORT_FIFO 

    ::rmdir(fifo_base(g_host_pid).c_str());
#endif 

    return 0;
}
