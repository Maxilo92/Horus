#ifndef REPLAY_PANEL_HPP
#define REPLAY_PANEL_HPP

#include "Blackboard.hpp"
#include <string>

class ReplayPanel {
public:
    ReplayPanel() = default;
    void render(Blackboard& blackboard);

private:
    std::string formatTime(double seconds);
};

#endif // REPLAY_PANEL_HPP
