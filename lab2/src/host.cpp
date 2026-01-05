#include "common.h"
#include "conn_iface.h"
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <cstdlib>
#include <fcntl.h>   // O_CREAT для sem_open

#ifdef TRANSPORT_SOCK
#include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
#endif

#ifdef TRANSPORT_MQ
static const char* kMode="mq";
#elif defined(TRANSPORT_FIFO)
static const char* kMode="fifo";
#elif defined(TRANSPORT_SOCK)
static const char* kMode="sock";
#else
#error "Transport macro not defined"
#endif

struct ClientCtx {
    uint32_t id=0;
    pid_t pid=0;
    bool alive=true;
    bool connected=false;
    std::unique_ptr<IConn> conn;
    sem_t* step_sem=nullptr;
    std::string nick;
    int last_round_seen=0;
};

static std::mutex g_mu;
static std::map<uint32_t, std::shared_ptr<ClientCtx>> g_clients;
static std::atomic<uint32_t> g_next_id{1};
static std::atomic<bool> g_stop{false};

static Logger* g_log=nullptr;
static pid_t g_host_pid=0;

static void send_sigusr2_with_id(pid_t cpid, uint32_t id) {
    union sigval sv; sv.sival_int = (int)id;
    sigqueue(cpid, SIGUSR2, sv);
}

#ifdef TRANSPORT_SOCK
static void accept_thread_sock(int listen_fd, int timeout_ms) {
  Logger log("/etc/logs/host.log","host");
  log.log("Listening on 0.0.0.0:5555");
  while (!g_stop.load()) {
    struct sockaddr_in sa; socklen_t sl=sizeof(sa);
    int cfd = accept(listen_fd, (struct sockaddr*)&sa, &sl);
    if (cfd<0) { usleep(100000); continue; }
    // Ждём HELLO от клиента
    Msg m{};
    std::unique_ptr<IConn> tmp(create_conn_sock_host_from_fd(cfd));
    if (!tmp) { close(cfd); continue; }
    if (!tmp->recv(&m, sizeof(m), timeout_ms)) {
      // не дождались — закрываем
      continue;
    }
    if (m.type!=HELLO) continue;
    uint32_t cid = m.client_id;
    std::shared_ptr<ClientCtx> ci;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_clients.find(cid);
      if (it!=g_clients.end()) ci = it->second;
    }
    if (!ci) continue;
    ci->nick = m.nick;
    ci->conn = std::move(tmp);
    ci->connected = true;

    // HELLO_ACK
    Msg ack{}; ack.type=HELLO_ACK; ack.client_id=cid; strncpy(ack.nick, "host", sizeof(ack.nick)-1);
    ci->conn->send(&ack, sizeof(ack), timeout_ms);
    log.log("Client connected via sock id="+std::to_string(cid));
  }
}
#endif

static void handle_new_clients(const CliArgsHost& /*args*/) {
    pid_t cpid;
    while (pop_pending_client(cpid)) {
        uint32_t id = g_next_id.fetch_add(1);
        auto ci = std::make_shared<ClientCtx>();
        ci->id = id; ci->pid = cpid; ci->alive = true;

        // Создать семафор
        std::string sname = sem_name_for(g_host_pid, id);
        sem_unlink(sname.c_str());
        ci->step_sem = sem_open(sname.c_str(), O_CREAT, 0666, 0);

#ifdef TRANSPORT_MQ
        ci->conn.reset(create_conn_mq_host(g_host_pid, id));
#elif defined(TRANSPORT_FIFO)
        ci->conn.reset(create_conn_fifo_host(g_host_pid, id));
#elif defined(TRANSPORT_SOCK)
        // Для sock conn создаётся после accept+HELLO
#endif
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_clients[id] = ci;
        }
        // Отправить SIGUSR2(id)
        send_sigusr2_with_id(cpid, id);
        g_log->log("Handshake: pid="+std::to_string(cpid)+" -> id="+std::to_string(id));
    }
}

static void broadcast_round_start(int round_no, int wolf_num, int timeout_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv: g_clients) {
        auto& c = *kv.second;
        if (!c.connected) continue;
        Msg m{}; m.type=ROUND_START; m.client_id=c.id; m.round_no=(uint32_t)round_no; m.value=(uint32_t)wolf_num;
        strncpy(m.nick, "host", sizeof(m.nick)-1);
        c.conn->send(&m, sizeof(m), timeout_ms);
    }
}

static void post_step_all() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv: g_clients) {
        auto& c = *kv.second;
        if (c.step_sem) sem_post(c.step_sem);
    }
}

static void send_result_to_all(int round_no, const std::map<uint32_t,ClientState>& states, int timeout_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv: g_clients) {
        auto& c=*kv.second;
        if (!c.connected) continue;
        Msg r{}; r.type=RESULT; r.client_id=c.id; r.round_no=(uint32_t)round_no;
        r.state = (uint32_t)states.at(c.id);
        strncpy(r.nick, "host", sizeof(r.nick)-1);
        c.conn->send(&r, sizeof(r), timeout_ms);
    }
}

static void send_game_over_all(int timeout_ms) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv: g_clients) {
        auto& c=*kv.second;
        if (!c.connected) continue;
        Msg m{}; m.type=GAME_OVER; m.client_id=c.id;
        c.conn->send(&m, sizeof(m), timeout_ms);
    }
}

int main(int argc, char** argv) {
    CliArgsHost args;
    parse_host_args(argc, argv, args);
    ensure_dir(args.log_dir);
    Logger log(args.log_dir+"/host.log","host");
    g_log=&log;
    install_sigusr1_host();
    rng_seed();

    g_host_pid=getpid();
    write_host_pid_file(g_host_pid);

    log.log(std::string("Host start pid=")+std::to_string(g_host_pid)+" mode="+kMode);

#ifdef TRANSPORT_SOCK
    int listen_fd = sock_listen_create(args.addr.c_str(), args.port);
  if (listen_fd<0) {
    log.err("socket listen failed");
    return 1;
  }
  std::thread accept_thr(accept_thread_sock, listen_fd, args.timeout_ms);
  accept_thr.detach();
#endif

    int rounds_all_dead_in_row=0;
    int round_no=0;

    while (!g_stop.load()) {
        handle_new_clients(args);

        // Подсчитать активных клиентов
        int n_clients=0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& kv: g_clients) if (kv.second->connected) n_clients++;
        }
        if (n_clients==0) {
            // Нет клиентов — подождать немного, но обрабатывать handshake
            usleep(200*1000);
            continue;
        }

        round_no++;
        // Число волка: попытка ручного ввода
        int wolf = try_read_number_3s();
        if (wolf<1) wolf = rng_uniform(1,100);
        log.log("Round "+std::to_string(round_no)+": wolf="+std::to_string(wolf)+" clients="+std::to_string(n_clients));

        broadcast_round_start(round_no, wolf, args.timeout_ms);
        post_step_all();

        // Сбор ходов
        std::map<uint32_t,int> turns;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& kv: g_clients) {
                auto& c=*kv.second;
                if (!c.connected) continue;
                Msg t{};
                if (c.conn->recv(&t, sizeof(t), args.timeout_ms) && t.type==TURN && t.round_no==(uint32_t)round_no) {
                    turns[c.id]=(int)t.value;
                } else {
                    // пропуск хода
                }
            }
        }

        // Подсчёт
        std::map<uint32_t,ClientState> new_state;
        int alive_cnt=0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& kv: g_clients) {
                auto& c=*kv.second;
                if (!c.connected) continue;
                int my = turns.count(c.id) ? turns[c.id] : -1;
                double n = (double)n_clients;
                bool hide = (my>=0) && (std::abs(my - wolf) <= (int)(70.0/n));
                bool rez  = (my>=0) && (std::abs(my - wolf) <= (int)(20.0/n));
                ClientState st = c.alive ? (hide ? ST_ALIVE : ST_DEAD) : (rez ? ST_ALIVE : ST_DEAD);
                c.alive = (st==ST_ALIVE);
                new_state[c.id]=st;
            }
            for (auto& kv: g_clients) if (kv.second->connected && kv.second->alive) alive_cnt++;
        }

        // Логи
        {
            std::string s="Turns:";
            for (auto& kv: turns) s += " c"+std::to_string(kv.first)+"="+std::to_string(kv.second);
            log.log(s);
            std::string st="Result:";
            for (auto& kv: new_state) st += " c"+std::to_string(kv.first)+"="+(kv.second==ST_ALIVE?"alive":"dead");
            log.log(st);
        }

        send_result_to_all(round_no, new_state, args.timeout_ms);

        if (alive_cnt==0) rounds_all_dead_in_row++; else rounds_all_dead_in_row=0;
        log.log("Alive after round: "+std::to_string(alive_cnt)+", all-dead-in-row="+std::to_string(rounds_all_dead_in_row));

        if (rounds_all_dead_in_row>=2) {
            log.log("Game over condition met");
            send_game_over_all(args.timeout_ms);
            break;
        }
        if (args.rounds>0 && round_no>=args.rounds) {
            log.log("Max rounds reached");
            send_game_over_all(args.timeout_ms);
            break;
        }
    }

    g_stop.store(true);
#ifdef TRANSPORT_SOCK
    // listen_fd закрывается в accept_thread_sock после detach? Он был передан по значению; закроем здесь:
  // (accept в отдельном потоке использует только cfd; listen_fd здесь можно закрыть)
  // Но у нас нет доступа к listen_fd тут — просто игнорируем, ОС очистит по exit процесса.
#endif

    // Очистка семафоров (best-effort)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv: g_clients) {
            auto& c=*kv.second;
            if (c.step_sem) {
                std::string sname = sem_name_for(g_host_pid, c.id);
                sem_close(c.step_sem);
                sem_unlink(sname.c_str());
            }
        }
    }

    return 0;
}
