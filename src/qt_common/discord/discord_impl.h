// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "discord.h"

#include <mutex>
#include <string>
#include <thread>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    std::string GetGameString(const std::string& title);
    void UpdateGameStatus(std::string_view game_url, bool use_default);

    std::string game_title{};
    s64 game_start_time{};
    Core::System& system;

    /// Checks for box art off the UI thread; the game is shown without it until the answer is in
    std::thread boxart_thread;
    /// Guards the generation, which tells a running box art check whether it is still current
    std::mutex presence_mutex;
    u64 presence_generation{};
};

} // namespace DiscordRPC
