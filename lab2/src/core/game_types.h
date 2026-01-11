#pragma once

#include <cstdint>

enum class KidStatus : std::uint8_t {
    ALIVE = 1,
    DEAD = 2
};

inline int absInt(int x) { return x < 0 ? -x : x; }


inline bool aliveHid(int kid, int wolf, int n) {
    const int d = absInt(kid - wolf);
    return d * n <= 70;
}

inline bool deadResurrect(int kid, int wolf, int n) {
    const int d = absInt(kid - wolf);
    return d * n <= 20;
}

inline const char *toStr(KidStatus st) {
    return (st == KidStatus::ALIVE) ? "alive" : "dead";
}
