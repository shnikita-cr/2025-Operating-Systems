#include "game_engine.h"
#include "game_types.h"
#include "protocol.h"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <time.h>

static long monoMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int hostOpTimeoutMs(int cliTimeoutMs) {
    int t = cliTimeoutMs;
    if (t <= 0) t = 5000;
    if (t > 5000) t = 5000;
    if (t > 4800) t = 4800;
    return t;
}

static int pickWolfNumber(Logger &log, int roundIndex) {
    log.info("Round " + std::to_string(roundIndex) +
             ": enter wolf number [1..100] within 3 seconds (otherwise random).");

    int v = 0;
    const bool ok = readIntWithinMs(3000, 1, 100, &v);
    if (ok) {
        log.info("Round " + std::to_string(roundIndex) + ": wolf chose manually: " + std::to_string(v));
        return v;
    }
    const int r = randIntInclusive(1, 100);
    log.info("Round " + std::to_string(roundIndex) + ": wolf chose random: " + std::to_string(r));
    return r;
}

static int pickKidNumber(KidStatus st) {
    return (st == KidStatus::ALIVE) ? randIntInclusive(1, 100) : randIntInclusive(1, 50);
}

static void joinWindowOnce(IHostSession &session, Logger &log) {
    auto clients = session.clientsSnapshot();
    if (clients.empty()) return;

    int lastCount = static_cast<int>(clients.size());
    auto start = std::chrono::steady_clock::now();
    auto maxDeadline = start + std::chrono::milliseconds(1500);
    auto quietDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);

    while (!stopRequested()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= maxDeadline) break;

        clients = session.clientsSnapshot();
        int c = static_cast<int>(clients.size());
        if (c != lastCount) {
            lastCount = c;
            quietDeadline = now + std::chrono::milliseconds(300);
            log.info("Join window: clients=" + std::to_string(lastCount));
        }

        if (now >= quietDeadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    clients = session.clientsSnapshot();
    log.info("Join window done: clients=" + std::to_string(static_cast<int>(clients.size())));
}

int runHost(IHostSession &session, const HostArgs &args, Logger &log) {
    log.info("Host starting game. rounds=" + std::to_string(args.rounds) +
             " timeoutMs=" + std::to_string(args.timeoutMs) +
             " nick=" + args.nick);

    session.startAccepting();
    log.info("Accepting clients. Waiting for the first client forever...");

    std::unordered_map<int, KidStatus> statusById;
    std::unordered_map<int, std::string> nickById;

    {
        auto last = std::chrono::steady_clock::now();
        while (!stopRequested()) {
            auto clients = session.clientsSnapshot();
            if (!clients.empty()) {
                joinWindowOnce(session, log);
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(5)) {
                log.info("Still waiting for clients...");
                last = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    int consecutiveAllDead = 0;
    int playedRounds = 0;

    while (!stopRequested() && playedRounds < args.rounds) {
        auto clients = session.clientsSnapshot();

        if (clients.empty()) {
            log.warn("No clients connected. Waiting again (round is not counted)...");
            auto last = std::chrono::steady_clock::now();
            while (!stopRequested()) {
                clients = session.clientsSnapshot();
                if (!clients.empty()) {
                    joinWindowOnce(session, log);
                    break;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::seconds(5)) {
                    log.info("Still waiting for clients...");
                    last = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            continue;
        }

        const int n = static_cast<int>(clients.size());

        for (const auto &link: clients) {
            if (!link) continue;
            const int cid = link->clientId();
            if (statusById.find(cid) == statusById.end()) {
                statusById[cid] = KidStatus::ALIVE;
                nickById[cid] = link->nick();
                log.info("Client joined: id=" + std::to_string(cid) + " nick=" + link->nick());
            }
        }

        const int round_index = playedRounds + 1;
        const int op_timeout = hostOpTimeoutMs(args.timeoutMs);

        const long t_round_start = monoMs();

        struct TurnOutcome {
            std::shared_ptr<IClientLink> link;
            int cid = 0;
            bool ok = false;
            int kid_number = 0;
            std::string err;

            long t_start = 0;
            long t_end = 0;
        };

        std::vector<TurnOutcome> outcomes;
        outcomes.reserve(clients.size());
        for (const auto &link: clients) {
            if (!link) continue;
            TurnOutcome o;
            o.link = link;
            o.cid = link->clientId();
            outcomes.push_back(o);
        }

        std::vector<std::thread> req_threads;
        req_threads.reserve(outcomes.size());

        const long t_turns_start = monoMs();
        for (size_t i = 0; i < outcomes.size(); i++) {
            req_threads.emplace_back([&, i] {
                auto &o = outcomes[i];
                if (!o.link) return;

                TurnRequest req;
                req.roundIndex = round_index;
                auto it = statusById.find(o.cid);
                req.currentStatus = (it != statusById.end()) ? it->second : KidStatus::ALIVE;

                TurnResponse resp;
                o.t_start = monoMs();
                o.ok = o.link->requestTurn(req, op_timeout, resp);
                o.t_end = monoMs();

                if (o.ok) {
                    o.kid_number = resp.number;
                } else {
                    o.err = o.link->lastError();
                }
            });
        }

        const long t_wolf_wait_start = monoMs();
        const int wolf = pickWolfNumber(log, round_index);
        const long t_wolf_ready = monoMs();

        for (auto &t: req_threads) t.join();
        const long t_turns_ready = monoMs();

        int hid_cnt = 0;
        int caught_cnt = 0;
        int resurrected_cnt = 0;

        struct ResultSend {
            std::shared_ptr<IClientLink> link;
            int cid = 0;
            ResultMessage msg;
            bool ok = false;
            std::string err;
        };
        std::vector<ResultSend> results;
        results.reserve(outcomes.size());

        std::string per_kid = "Round " + std::to_string(round_index) +
                              ": clients=" + std::to_string(n) +
                              " wolf=" + std::to_string(wolf) + " | ";


        for (auto &o: outcomes) {
            if (!o.link) continue;
            const int cid = o.cid;
            const std::string knick = (nickById.count(cid) ? nickById[cid] : o.link->nick());

            if (!o.ok) {
                log.error("TURN_RESP failed: id=" + std::to_string(cid) +
                          " nick=" + knick +
                          " err='" + o.err + "'" +
                          " t=" + std::to_string(o.t_end - o.t_start) + "ms. Disconnecting.");
                o.link->disconnect();
                statusById.erase(cid);
                nickById.erase(cid);
                continue;
            }

            const int kid_num = o.kid_number;
            const KidStatus prev = statusById[cid];
            KidStatus next = prev;

            bool hid = false;
            bool resurrected = false;

            if (prev == KidStatus::ALIVE) {
                hid = aliveHid(kid_num, wolf, n);
                if (hid) {
                    next = KidStatus::ALIVE;
                    hid_cnt++;
                } else {
                    next = KidStatus::DEAD;
                    caught_cnt++;
                }
            } else {
                resurrected = deadResurrect(kid_num, wolf, n);
                if (resurrected) {
                    next = KidStatus::ALIVE;
                    resurrected_cnt++;
                } else {
                    next = KidStatus::DEAD;
                }
            }

            statusById[cid] = next;

            per_kid += knick + "(id=" + std::to_string(cid) + " " + toStr(prev) + ")->" +
                       std::to_string(kid_num) + " => " + toStr(next) + "; ";

            ResultSend rs;
            rs.link = o.link;
            rs.cid = cid;
            rs.msg.roundIndex = round_index;
            rs.msg.wolfNumber = wolf;
            rs.msg.kidNumber = kid_num;
            rs.msg.hid = hid;
            rs.msg.resurrected = resurrected;
            rs.msg.newStatus = next;
            rs.msg.gameOver = false;
            results.push_back(rs);
        }

        int alive_cnt = 0;
        int dead_cnt = 0;
        for (const auto &kv: statusById) {
            (kv.second == KidStatus::ALIVE) ? alive_cnt++ : dead_cnt++;
        }

        log.info(per_kid);
        log.info("Round " + std::to_string(round_index) +
                 " summary: hid=" + std::to_string(hid_cnt) +
                 " caught=" + std::to_string(caught_cnt) +
                 " resurrected=" + std::to_string(resurrected_cnt) +
                 " alive=" + std::to_string(alive_cnt) +
                 " dead=" + std::to_string(dead_cnt));

        const bool all_dead = (alive_cnt == 0 && dead_cnt > 0);
        const int new_consecutive_all_dead = all_dead ? (consecutiveAllDead + 1) : 0;
        const bool game_over_now = (new_consecutive_all_dead >= 2);


        for (auto &rs: results) rs.msg.gameOver = game_over_now;

        std::vector<std::thread> send_threads;
        send_threads.reserve(results.size());
        const long t_results_start = monoMs();

        for (size_t i = 0; i < results.size(); i++) {
            send_threads.emplace_back([&, i] {
                auto &rs = results[i];
                if (!rs.link) return;

                if (rs.link->sendResult(rs.msg, op_timeout)) {
                    rs.ok = true;
                    return;
                }
                rs.ok = false;
                rs.err = rs.link->lastError();
            });
        }

        for (auto &t: send_threads) t.join();
        const long t_results_done = monoMs();

        for (auto &rs: results) {
            if (!rs.ok && rs.link) {
                const int cid = rs.cid;
                const std::string knick = (nickById.count(cid) ? nickById[cid] : rs.link->nick());
                log.error("sendResult failed: id=" + std::to_string(cid) +
                          " nick=" + knick +
                          " err='" + rs.err + "'. Disconnecting.");
                rs.link->disconnect();
                statusById.erase(cid);
                nickById.erase(cid);
            }
        }

        const long t_round_end = monoMs();

        log.info("Round " + std::to_string(round_index) + " timing(ms): " +
                 "wolf_wait=" + std::to_string(t_wolf_ready - t_wolf_wait_start) +
                 " turns_total=" + std::to_string(t_turns_ready - t_turns_start) +
                 " results_send=" + std::to_string(t_results_done - t_results_start) +
                 " round_total=" + std::to_string(t_round_end - t_round_start));

        playedRounds++;

        consecutiveAllDead = new_consecutive_all_dead;
        if (game_over_now) {
            log.warn("Game over: all kids dead for 2 consecutive rounds.");
            break;
        }
    }

    log.info("Host shutting down...");
    session.shutdown();
    log.info("Host finished.");
    return 0;
}

int runClient(IClientSession &session, const ClientArgs &args, Logger &log) {
    log.info("Client starting. hostPid=" + std::to_string(args.hostPid) +
             " timeoutMs=" + std::to_string(args.timeoutMs) +
             " nick=" + args.nick);

    if (!session.connect()) {
        log.error("connect() failed: err='" + session.lastError() + "'");
        session.shutdown();
        return 4;
    }

    log.info("Connected.");

    KidStatus status = KidStatus::ALIVE;

    while (!stopRequested()) {
        TurnRequest req;
        if (!session.waitTurnRequest(args.timeoutMs, req)) {
            log.warn("waitTurnRequest() failed/timeout: err='" + session.lastError() + "'. Exiting.");
            break;
        }

        status = req.currentStatus;

        TurnResponse resp;
        resp.number = pickKidNumber(status);

        log.info("Round " + std::to_string(req.roundIndex) +
                 ": status=" + std::string(toStr(status)) +
                 " picked=" + std::to_string(resp.number));

        if (!session.sendTurn(resp, args.timeoutMs)) {
            log.error("sendTurn() failed: err='" + session.lastError() + "'. Exiting.");
            break;
        }

        ResultMessage res;
        if (!session.waitResult(args.timeoutMs, res)) {
            log.error("waitResult() failed/timeout: err='" + session.lastError() + "'. Exiting.");
            break;
        }

        status = res.newStatus;

        log.info("Round " + std::to_string(res.roundIndex) +
                 ": wolf=" + std::to_string(res.wolfNumber) +
                 " kid=" + std::to_string(res.kidNumber) +
                 " hid=" + std::string(res.hid ? "true" : "false") +
                 " resurrected=" + std::string(res.resurrected ? "true" : "false") +
                 " => newStatus=" + std::string(toStr(status)));

        if (res.gameOver) {
            log.warn("Game over received from host. Exiting.");
            break;
        }
    }

    log.info("Client shutting down...");
    session.shutdown();
    log.info("Client finished.");
    return 0;
}
