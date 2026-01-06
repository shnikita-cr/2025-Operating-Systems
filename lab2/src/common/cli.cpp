#include "cli.h"

#include <cstdlib>
#include <sstream>
#include <unordered_set>

static bool myStartsWith(const std::string &s, const std::string &pref) {
    return s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
}

static bool myParseInt(const std::string &s, int *out) {
    if (!out) return false;
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    if (v < -2147483648LL || v > 2147483647LL) return false;
    *out = static_cast<int>(v);
    return true;
}

static bool myGetValue(int argc, char **argv, int &i, std::string &outVal) {
    const std::string a = argv[i];
    const auto eq = a.find('=');
    if (eq != std::string::npos) {
        outVal = a.substr(eq + 1);
        return true;
    }
    if (i + 1 >= argc) return false;
    outVal = argv[++i];
    return true;
}

static bool myCheckPositive(const char *name, int v, std::string *err) {
    if (v <= 0) {
        if (err) {
            std::ostringstream oss;
            oss << "Invalid " << name << ": must be > 0";
            *err = oss.str();
        }
        return false;
    }
    return true;
}

bool parseHostArgs(int argc, char **argv, HostArgs &out, std::string *err) {
    std::unordered_set<std::string> known = {
            "--rounds", "--timeout-ms", "--log-dir", "--nick",
            "--addr", "--port",
            "--mode"
    };

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (!myStartsWith(a, "--")) {
            if (err) *err = "Unexpected argument (expected --key ...): " + a;
            return false;
        }


        std::string key = a;
        const auto eq = key.find('=');
        if (eq != std::string::npos) key = key.substr(0, eq);

        if (known.find(key) == known.end()) {
            if (err) *err = "Unknown option: " + key;
            return false;
        }

        if (key == "--mode") {
            std::string dummy;
            (void) myGetValue(argc, argv, i, dummy);
            continue;
        }

        std::string val;
        if (!myGetValue(argc, argv, i, val)) {
            if (err) *err = "Missing value for option: " + key;
            return false;
        }

        if (key == "--rounds") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1) {
                if (err) *err = "Invalid --rounds: " + val;
                return false;
            }
            out.rounds = v;
        } else if (key == "--timeout-ms") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1) {
                if (err) *err = "Invalid --timeout-ms: " + val;
                return false;
            }
            out.timeoutMs = v;
        } else if (key == "--log-dir") {
            out.logDir = val;
        } else if (key == "--nick") {
            out.nick = val;
        } else if (key == "--addr") {
            out.addr = val;
        } else if (key == "--port") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1 || v > 65535) {
                if (err) *err = "Invalid --port: " + val;
                return false;
            }
            out.port = v;
        }
    }

    if (!myCheckPositive("--rounds", out.rounds, err)) return false;
    if (!myCheckPositive("--timeout-ms", out.timeoutMs, err)) return false;
    if (out.logDir.empty()) {
        if (err) *err = "--log-dir is required (must be non-empty)";
        return false;
    }
    if (out.nick.empty()) {
        if (err) *err = "--nick is required (must be non-empty)";
        return false;
    }
    return true;
}

bool parseClientArgs(int argc, char **argv, ClientArgs &out, std::string *err) {
    std::unordered_set<std::string> known = {
            "--host-pid", "--timeout-ms", "--log-dir", "--nick",
            "--host-addr", "--host-port",
            "--mode"
    };

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (!myStartsWith(a, "--")) {
            if (err) *err = "Unexpected argument (expected --key ...): " + a;
            return false;
        }

        std::string key = a;
        const auto eq = key.find('=');
        if (eq != std::string::npos) key = key.substr(0, eq);

        if (known.find(key) == known.end()) {
            if (err) *err = "Unknown option: " + key;
            return false;
        }

        if (key == "--mode") {
            std::string dummy;
            (void) myGetValue(argc, argv, i, dummy);
            continue;
        }

        std::string val;
        if (!myGetValue(argc, argv, i, val)) {
            if (err) *err = "Missing value for option: " + key;
            return false;
        }

        if (key == "--host-pid") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1) {
                if (err) *err = "Invalid --host-pid: " + val;
                return false;
            }
            out.hostPid = v;
        } else if (key == "--timeout-ms") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1) {
                if (err) *err = "Invalid --timeout-ms: " + val;
                return false;
            }
            out.timeoutMs = v;
        } else if (key == "--log-dir") {
            out.logDir = val;
        } else if (key == "--nick") {
            out.nick = val;
        } else if (key == "--host-addr") {
            out.hostAddr = val;
        } else if (key == "--host-port") {
            int v = 0;
            if (!myParseInt(val, &v) || v < 1 || v > 65535) {
                if (err) *err = "Invalid --host-port: " + val;
                return false;
            }
            out.hostPort = v;
        }
    }

    if (!myCheckPositive("--timeout-ms", out.timeoutMs, err)) return false;
    if (out.logDir.empty()) {
        if (err) *err = "--log-dir is required (must be non-empty)";
        return false;
    }
    if (out.nick.empty()) {
        if (err) *err = "--nick is required (must be non-empty)";
        return false;
    }
    if (!myCheckPositive("--host-pid", out.hostPid, err)) return false;

    return true;
}
