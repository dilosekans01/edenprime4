// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <string>

#include <QEventLoop>
#include <boost/algorithm/string/replace.hpp>
#include "common/httplib.h"

#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/loader/loader.h"

#include "discord_impl.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace DiscordRPC {

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for Eden, it's used for images and the
    // application name
    Discord_Initialize("1397286652128264252", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Pause();
    if (boxart_thread.joinable()) {
        boxart_thread.join();
    }
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    std::scoped_lock lock{presence_mutex};
    ++presence_generation;
    Discord_ClearPresence();
}

std::string DiscordImpl::GetGameString(const std::string& title) {
    // Convert to lowercase
    std::string icon_name = Common::ToLower(title);

    // Replace spaces with dashes
    std::replace(icon_name.begin(), icon_name.end(), ' ', '-');
    boost::replace_all(icon_name, "é", "e");

    // Remove non-alphanumeric characters but keep dashes
    std::erase_if(icon_name, [](char c) { return !std::isalnum(c) && c != '-'; });

    // Remove dashes from the start and end of the string
    icon_name.erase(icon_name.begin(), std::find_if(icon_name.begin(), icon_name.end(),
                                                    [](int ch) { return ch != '-'; }));
    icon_name.erase(
        std::find_if(icon_name.rbegin(), icon_name.rend(), [](int ch) { return ch != '-'; }).base(),
        icon_name.end());

    // Remove double dashes
    icon_name.erase(std::unique(icon_name.begin(), icon_name.end(),
                                [](char a, char b) { return a == '-' && b == '-'; }),
                    icon_name.end());

    return icon_name;
}

static constexpr char DEFAULT_DISCORD_TEXT[] = "Eden is an emulator for the Nintendo Switch";
static constexpr char DEFAULT_DISCORD_IMAGE[] =
    "https://git.eden-emu.dev/eden-emu/eden/raw/branch/master/dist/qt_themes/default/icons/256x256/"
    "eden.png";

// eden-kledd: Metroid Prime 4: Beyond has no art in the Eden project's collection
static constexpr u64 METROID_PRIME_4_PROGRAM_ID = 0x010019A01E2F2000;
static constexpr char METROID_PRIME_4_IMAGE[] =
    "https://cdn2.steamgriddb.com/icon/d8728c90cd89e0862fba0786ffa6b195/32/256x256.png";

void DiscordImpl::UpdateGameStatus(std::string_view game_url, bool has_boxart) {
    const std::string url = std::string{has_boxart ? game_url : DEFAULT_DISCORD_IMAGE};
    DiscordRichPresence presence{};

    presence.largeImageKey = url.c_str();
    presence.largeImageText = game_title.c_str();
    presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
    presence.smallImageText = DEFAULT_DISCORD_TEXT;
    presence.state = game_title.c_str();
    presence.status_display_type = DiscordStatusDisplayType_State;
    presence.details = "Currently in game";
    presence.startTimestamp = game_start_time;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::Update() {
    u64 generation{};
    {
        std::scoped_lock lock{presence_mutex};
        generation = ++presence_generation;
    }
    // The check also reads the title and the start time, which change below
    if (boxart_thread.joinable()) {
        boxart_thread.join();
    }

    if (system.IsPoweredOn()) {
        system.GetAppLoader().ReadTitle(game_title);
        game_start_time = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        {
            const bool known_art =
                system.GetApplicationProcessProgramID() == METROID_PRIME_4_PROGRAM_ID;
            std::scoped_lock lock{presence_mutex};
            UpdateGameStatus(known_art ? METROID_PRIME_4_IMAGE : "", known_art);
            if (known_art) {
                return;
            }
        }

        // Used to format Icon URL for yuzu website game compatibility page
        constexpr char boxart_host[] = "raw.githubusercontent.com";
        std::string boxart_path = fmt::format(
            "/eden-emulator/boxart/refs/heads/master/img/{}.png", GetGameString(game_title));

        // This runs on the UI thread while a game boots, so the network request goes elsewhere
        boxart_thread = std::thread([this, generation, boxart_host,
                                     boxart_path = std::move(boxart_path)] {
            httplib::SSLClient client(boxart_host);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);
            client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
            client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

            const auto res = client.Head(boxart_path);
            if (!res || res->status != 200) {
                return;
            }
            std::scoped_lock lock{presence_mutex};
            if (generation == presence_generation) {
                UpdateGameStatus(fmt::format("https://{}{}", boxart_host, boxart_path), true);
            }
        });
        return;
    }

    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    DiscordRichPresence presence{};
    presence.largeImageKey = DEFAULT_DISCORD_IMAGE;
    presence.largeImageText = DEFAULT_DISCORD_TEXT;
    presence.status_display_type = DiscordStatusDisplayType_Name;
    presence.details = "Currently not in game";
    presence.startTimestamp = start_time;
    std::scoped_lock lock{presence_mutex};
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC
