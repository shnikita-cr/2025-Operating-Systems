#include "common.h"
#include "conn_iface.h"
#include <signal.h>
#include <string.h>
#include <semaphore.h>
#include <unistd.h>
#include <memory>
#include <pthread.h>

static int wait_sigusr2_get_id(int timeout_ms, int& out_id) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    // Блокируем SIGUSR2 в текущем потоке и ждём его целенаправленно
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    struct timespec ts = ts_after_ms(timeout_ms);
    siginfo_t si{};
    int r = sigtimedwait(&set, &si, &ts);
    if (r==SIGUSR2) { out_id = si.si_value.sival_int; return 0; }
    return -1;
}

int main(int argc, char** argv) {
    CliArgsClient args;
    if (!parse_client_args(argc, argv, args)) {
        fprintf(stderr, "Usage: client_* --host-pid <pid> [--nick N] [--timeout-ms M] [--log-dir D] [--host-addr A --host-port P]\n");
        return 2;
    }
    ensure_dir(args.log_dir);
    Logger log(args.log_dir+"/client_"+std::to_string(getpid())+".log","client");
    rng_seed();

    // Handshake step 1: SIGUSR1 -> host
    kill(args.host_pid, SIGUSR1);

    // Wait SIGUSR2 with client_id
    int client_id=-1;
    if (wait_sigusr2_get_id(args.timeout_ms, client_id)!=0 || client_id<=0) {
        log.err("Handshake failed (SIGUSR2 timeout)");
        return 1;
    }

    // Open semaphore
    std::string sname = sem_name_for(args.host_pid, (uint32_t)client_id);
    sem_t* step = sem_open(sname.c_str(), 0);
    if (!step) {
        log.err("sem_open failed");
        return 1;
    }

    std::unique_ptr<IConn> conn;

#ifdef TRANSPORT_MQ
    conn.reset(create_conn_mq_client(args.host_pid, (uint32_t)client_id));
#elif defined(TRANSPORT_FIFO)
    conn.reset(create_conn_fifo_client(args.host_pid, (uint32_t)client_id));
#elif defined(TRANSPORT_SOCK)
    conn.reset(create_conn_sock_client_connect(args.host_addr.c_str(), args.host_port));
#endif
    if (!conn) { log.err("Connection create failed"); return 1; }

    // HELLO
    Msg h{}; h.type=HELLO; h.client_id=(uint32_t)client_id;
    strncpy(h.nick, args.nick.c_str(), sizeof(h.nick)-1);
    if (!conn->send(&h, sizeof(h), args.timeout_ms)) {
        log.err("HELLO send failed"); return 1;
    }
    Msg ack{};
    if (!conn->recv(&ack, sizeof(ack), args.timeout_ms) || ack.type!=HELLO_ACK) {
        log.err("HELLO_ACK recv failed"); return 1;
    }

    bool alive=true;
    int round_no=0;

    while (true) {
        // Ждём ROUND_START
        Msg rs{};
        if (!conn->recv(&rs, sizeof(rs), args.timeout_ms)) {
            log.err("ROUND_START timeout");
            break;
        }
        if (rs.type==GAME_OVER) {
            log.log("GAME_OVER received");
            break;
        }
        if (rs.type!=ROUND_START) {
            // пропускаем постороннее
            continue;
        }
        round_no = (int)rs.round_no;

        // Барьер
        struct timespec ts = ts_after_ms(args.timeout_ms);
        if (sem_timedwait(step, &ts)!=0) {
            log.err("sem_timedwait timeout");
            break;
        }

        // Сгенерировать ход
        int v = alive ? rng_uniform(1,100) : rng_uniform(1,50);
        Msg t{}; t.type=TURN; t.client_id=(uint32_t)client_id; t.round_no=rs.round_no; t.value=(uint32_t)v;
        strncpy(t.nick, args.nick.c_str(), sizeof(t.nick)-1);
        conn->send(&t, sizeof(t), args.timeout_ms);

        // Получить RESULT
        Msg r{};
        if (!conn->recv(&r, sizeof(r), args.timeout_ms)) {
            log.err("RESULT timeout");
            break;
        }
        if (r.type==GAME_OVER) {
            log.log("GAME_OVER received");
            break;
        }
        if (r.type==RESULT && (int)r.round_no==round_no) {
            alive = (r.state==ST_ALIVE);
            log.log(std::string("Round ")+std::to_string(round_no)+": sent="+std::to_string(v)+" => "+(alive?"alive":"dead"));
        }
    }

    sem_close(step);
    return 0;
}
