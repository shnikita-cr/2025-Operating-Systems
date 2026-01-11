#pragma once

#include "game_types.h"

struct TurnRequest {
    int roundIndex = 0;
    KidStatus currentStatus = KidStatus::ALIVE;
};

struct TurnResponse {
    int number = 0;
};

struct ResultMessage {
    int roundIndex = 0;

    int wolfNumber = 0;
    int kidNumber = 0;

    bool hid = false;
    bool resurrected = false;

    KidStatus newStatus = KidStatus::ALIVE;
    bool gameOver = false;
};
