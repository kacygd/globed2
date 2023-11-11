#include "network_manager.hpp"

#include <util/net.hpp>
#include <data/packets/all.hpp>
#include <managers/server_manager.hpp>
#include <managers/error_queues.hpp>

namespace log = geode::log;

GLOBED_SINGLETON_DEF(NetworkManager)

NetworkManager::NetworkManager() {
    util::net::initialize();

    if (!socket.create()) util::net::throwLastError();

    threadMain = std::thread(&NetworkManager::threadMainFunc, this);
    threadRecv = std::thread(&NetworkManager::threadRecvFunc, this);
    threadTasks = std::thread(&NetworkManager::threadTasksFunc, this);
    threadPingRecv = std::thread(&NetworkManager::threadPingRecvFunc, this);
}

NetworkManager::~NetworkManager() {
    // cleanup left packets
    _running = false;

    log::debug("waiting for threads to die..");

    if (threadMain.joinable()) threadMain.join();

    if (socket.connected) {
        log::debug("disconnecting from the server..");
        // todo
    }

    log::debug("cleaning up..");
    for (Packet* packet : packetQueue.popAll()) {
        delete packet;
    }

    util::net::cleanup();
    log::debug("Goodbye!");
}

void NetworkManager::connect(const std::string& addr, unsigned short port) {
    GLOBED_ASSERT(socket.connect(addr, port), "failed to connect to the server")
    socket.createBox();
}

void NetworkManager::disconnect() {
    if (!connected()) {
        return;
    }

    _established = false;

    socket.disconnect();
    socket.cleanupBox();
}

void NetworkManager::send(Packet* packet) {
    GLOBED_ASSERT(socket.connected, "tried to send a packet while disconnected")
    packetQueue.push(packet);
}

void NetworkManager::addListener(packetid_t id, PacketCallback callback) {
    (*listeners.lock())[id] = callback;
}

void NetworkManager::removeListener(packetid_t id) {
    listeners.lock()->erase(id);
}

void NetworkManager::removeAllListeners() {
    listeners.lock()->clear();
}

// tasks

void NetworkManager::taskPingServers() {
    taskQueue.push(NetworkThreadTask::PingServers);
}

// threads

void NetworkManager::threadMainFunc() {
    while (_running) {
        if (!packetQueue.waitForMessages(std::chrono::seconds(1))) {
            continue;
        }

        auto messages = packetQueue.popAll();

        for (Packet* packet : messages) {
            try {
                socket.sendPacket(packet);
            } catch (const std::exception& e) {
                ErrorQueues::get().error(e.what());
            }

            delete packet;
        }
    }
}

void NetworkManager::threadRecvFunc() {
    while (_running) {
        if (!socket.poll(1000)) {
            continue;
        }

        std::shared_ptr<Packet> packet;

        try {
            packet = socket.recvPacket();
        } catch (const std::exception& e) {
            ErrorQueues::get().warn(fmt::format("failed to receive a packet: {}", e.what()));
            continue;
        }

        packetid_t packetId = packet->getPacketId();

        // we have predefined handlers for connection related packets
        if (packetId == 20001) {
            auto packet_ = static_cast<CryptoHandshakeResponsePacket*>(packet.get());
            socket.box->setPeerKey(packet_->data.key.data());
            _established = true;

            continue;
        } else if (packetId == 20002) {
            auto packet_ = static_cast<KeepaliveResponsePacket*>(packet.get());
            // ?
            continue;
        } else if (packetId == 20003) {
            auto packet_ = static_cast<ServerDisconnectPacket*>(packet.get());
            ErrorQueues::get().error(fmt::format("You have been disconnected from the active server.\n\nReason: <cy>{}</c>", packet_->message));
            this->disconnect();
            continue;
        }

        // this is scary
        geode::Loader::get()->queueInMainThread([this, packetId, packet]() {
            auto listeners_ = this->listeners.lock();
            if (!listeners_->contains(packetId)) {
                log::warn("Unhandled packet: {}", packetId);
            } else {
                // xd
                (*listeners_)[packetId](packet);
            }
        });
    }
}

void NetworkManager::threadTasksFunc() {
    while (_running) {
        if (!taskQueue.waitForMessages(std::chrono::seconds(1))) {
            continue;
        }

        for (auto task : taskQueue.popAll()) {
            switch (task) {
            case NetworkThreadTask::PingServers: {
                for (auto& [serverId, address] : GlobedServerManager::get().getServerAddresses()) {
                    auto pingId = GlobedServerManager::get().addPendingPing(serverId);
                    Packet* packet = PingPacket::create(pingId);

                    try {
                        pingSocket.connect(address.ip, address.port);
                        pingSocket.sendPacket(packet);
                        pingSocket.disconnect();
                    } catch (const std::exception& e) {
                        ErrorQueues::get().warn(e.what());
                    }

                    delete packet;
                }
            }
            }
        }
    }
}

void NetworkManager::threadPingRecvFunc() {
    while (_running) {
        // poll the ping socket, process all responses
        if (!pingSocket.poll(1000)) {
            continue;
        }

        try {
            auto packet = pingSocket.recvPacket();
            if (PingResponsePacket* pingr = dynamic_cast<PingResponsePacket*>(packet.get())) {
                GlobedServerManager::get().recordPingResponse(pingr->id, pingr->playerCount);
            }
        } catch (const std::exception& e) {
            ErrorQueues::get().warn(fmt::format("error pinging a server: {}", e.what()));
        }
    }
}

bool NetworkManager::connected() {
    return socket.connected;
}

bool NetworkManager::established() {
    return socket.connected && _established;
}