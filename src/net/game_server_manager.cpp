#include "game_server_manager.hpp"
#include <util/time.hpp>
#include <util/rng.hpp>

GLOBED_SINGLETON_GET(GameServerManager)

GameServerManager::GameServerManager() {

}

uint32_t GameServerManager::addPendingPing(const std::string& serverId) {
    uint32_t pingId = util::rng::Random::get().generate<uint32_t>();

    auto& gsi = servers.write()->at(serverId);
    gsi.pendingPings[pingId] = util::time::now();

    return pingId;
}

void GameServerManager::recordPingResponse(uint32_t pingId, uint32_t playerCount) {
    auto servers_ = servers.write();
    for (auto& server : util::collections::mapValues(*servers_)) {
        if (server.pendingPings.contains(pingId)) {
            auto start = server.pendingPings.at(pingId);
            auto timeTook = util::time::now() - start;
            server.ping = timeTook;
            server.playerCount = playerCount;
            server.pingHistory.push(timeTook);
            server.pendingPings.erase(pingId);
            return;
        }
    }

    geode::log::warn("Ping ID doesn't exist in any known server: {}", pingId);
}

GameServerView GameServerManager::getServerView(const std::string& serverId) {
    auto& gsi = servers.read()->at(serverId);
    return GameServerView {
        .ping = gsi.ping,
        .playerCount = gsi.playerCount
    };
}

std::vector<std::chrono::milliseconds> GameServerManager::getPingHistory(const std::string& serverId) {
    auto& gsi = servers.read()->at(serverId);
    return gsi.pingHistory.extract();
}

std::unordered_map<std::string, GameServerAddress> GameServerManager::getServerAddresses() {
    std::unordered_map<std::string, GameServerAddress> out;
    auto servers_ = servers.read();
    for (const auto& [serverId, gsi] : *servers_) {
        out[serverId] = gsi.address;
    }

    return out;
}