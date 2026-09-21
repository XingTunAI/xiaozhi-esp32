#pragma once

#include <string>
#include "scale_state.h"

enum class BoardScaleMode { Off, Simulated, Hardware };
struct BoardScaleStatus {
    BoardScaleMode mode = BoardScaleMode::Off;
    ScaleState measurement;
};

// Channel indices are 0/1 in C++, 1/2 in the physical console.
BoardScaleStatus GetBoardScaleStatus(unsigned channel);
bool HandleBoardScaleCommand(const std::string& command);
