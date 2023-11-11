use std::{
    net::{SocketAddr, SocketAddrV4},
    sync::Arc,
};

use anyhow::anyhow;
use crypto_box::{aead::OsRng, SecretKey};
use globed_shared::GameServerBootData;
use rustc_hash::FxHashMap;
#[allow(unused_imports)]
use tokio::sync::oneshot; // no way

use log::{debug, info, warn};
use tokio::{net::UdpSocket, sync::RwLock};

use crate::{
    data::packets::server::VoiceBroadcastPacket,
    server_thread::{GameServerThread, ServerThreadMessage},
    state::ServerState,
};

pub struct GameServer {
    pub address: String,
    pub state: ServerState,
    pub socket: Arc<UdpSocket>,
    pub threads: RwLock<FxHashMap<SocketAddrV4, Arc<GameServerThread>>>,
    pub secret_key: SecretKey,
    pub central_conf: GameServerBootData,
}

impl GameServer {
    pub async fn new(
        address: String,
        state: ServerState,
        central_conf: GameServerBootData,
    ) -> Self {
        let secret_key = SecretKey::generate(&mut OsRng);

        Self {
            address: address.clone(),
            state,
            socket: Arc::new(UdpSocket::bind(&address).await.unwrap()),
            threads: RwLock::new(FxHashMap::default()),
            secret_key,
            central_conf,
        }
    }

    pub async fn run(&'static self) -> anyhow::Result<()> {
        let mut buf = [0u8; 65536];

        info!("Server launched on {}", self.address);

        loop {
            match self.recv_and_handle(&mut buf).await {
                Ok(_) => {}
                Err(err) => {
                    warn!("Failed to handle a packet: {}", err.to_string());
                }
            }
        }
    }

    async fn recv_and_handle(&'static self, buf: &mut [u8]) -> anyhow::Result<()> {
        let (len, peer) = self.socket.recv_from(buf).await?;

        let peer = match peer {
            SocketAddr::V6(_) => return Err(anyhow!("rejecting request from ipv6 host")),
            SocketAddr::V4(x) => x,
        };

        let threads = self.threads.read().await;
        let has_thread = threads.contains_key(&peer);

        if has_thread {
            threads
                .get(&peer)
                .unwrap()
                .send_message(ServerThreadMessage::Packet(buf[..len].to_vec()))
                .await?;
        } else {
            drop(threads);
            let mut threads = self.threads.write().await;

            debug!("creating new thread for {}", peer);

            let thread = Arc::new(GameServerThread::new(
                self.state.clone(),
                peer,
                self.socket.clone(),
                self.secret_key.clone(),
                self,
            ));
            let thread_cl = thread.clone();

            tokio::spawn(async move {
                match thread.run().await {
                    Ok(_) => {
                        // remove the thread from the list of threads in order to cleanup
                        debug!("removing dead client: {}", peer);
                        self.remove_client(&peer).await;
                    }
                    Err(err) => {
                        warn!("Client thread died: {}", err.to_string());
                    }
                };
            });

            thread_cl
                .send_message(ServerThreadMessage::Packet(buf[..len].to_vec()))
                .await?;

            threads.insert(peer, thread_cl);
        }

        Ok(())
    }

    pub async fn broadcast_voice_packet(
        &'static self,
        vpkt: &VoiceBroadcastPacket,
    ) -> anyhow::Result<()> {
        // todo dont send it to every single thread in existence
        let threads = self.threads.read().await;
        for thread in threads.values() {
            let packet = vpkt.clone();
            thread
                .send_message(ServerThreadMessage::BroadcastVoice(packet))
                .await?;
        }

        Ok(())
    }

    async fn remove_client(&'static self, key: &SocketAddrV4) {
        self.threads.write().await.remove(key);
    }
}
