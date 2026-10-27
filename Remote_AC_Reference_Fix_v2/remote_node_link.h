#pragma once
// Included AFTER ConfigGuard, savedRoomId, sensorState and sensorMux.
// Only this worker owns pairing/radio state. No MQTT or I2C runs in it.
#include <freertos/queue.h>
#include <string.h>
#include <stddef.h>

enum class NodeEventKind : uint8_t { PACKET, OPEN, REPLACE, APPROVE, REJECT, UNPAIR };
struct NodeEvent {
  NodeEventKind kind = NodeEventKind::PACKET;
  uint8_t mac[6] = {};
  uint8_t bytes[64] = {};
  uint8_t len = 0;
  uint32_t receivedAt = 0;
};
QueueHandle_t nodeQueue = nullptr;
std::atomic<bool> radioReady{false};
std::atomic<bool> flagNewCandidate{false};
std::atomic<bool> flagPairingSuccess{false};
uint8_t candidateNoticeMac[6] = {}; // Protected by sensorMux.
uint8_t pairedNodeMac[6] = {};
uint8_t candidateNodeMac[6] = {};
bool nodeIsPaired = false;
bool nodeUnpairPending = false;
bool pairingOpen = false;
bool candidatePresent = false;
bool candidateApproved = false;
bool candidateAlreadyPaired = false;
bool candidateHasTelemetry = false;
NodeEvent candidateTelemetry;
uint32_t pairingOpenedAt = 0;
uint32_t candidateSession = 0;
uint32_t committedSession = 0;
uint32_t lastOfferAt = 0;
std::atomic<uint32_t> nodeRxFrames{0}, nodeRxBadPacket{0}, nodeRxQueueFull{0};
std::atomic<uint32_t> nodeRxUnpaired{0}, nodeRxWrongMac{0}, nodeRxAccepted{0};
std::atomic<uint32_t> nodeRxInvalid{0}, nodeRxDuplicate{0};
std::atomic<bool> pairedNodeContactSeen{false};
std::atomic<uint32_t> lastPairedNodeContactAt{0};

void printNodeMac(const char* label, const uint8_t* mac) {
  Serial.printf("%s%02X:%02X:%02X:%02X:%02X:%02X\n", label,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool validNodeMac(const uint8_t* mac) {
  uint8_t bits = 0;
  for (uint8_t i = 0; i < 6; ++i) bits |= mac[i];
  return bits != 0 && !(mac[0] & 1);
}
bool ensureNodePeer(const uint8_t* mac) {
  if (!validNodeMac(mac)) return false;
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}
void replyHeader(PacketHeader& out, uint8_t type, const PacketHeader& request) {
  out = PacketHeader{};
  out.magic = AACS_MAGIC; out.version = PROTOCOL_VERSION; out.msg_type = type;
  out.session_id = request.session_id; out.seq_num = request.seq_num;
}
bool sendNodePacket(const uint8_t* mac, const void* bytes, size_t len) {
  return ensureNodePeer(mac) && esp_now_send(mac, static_cast<const uint8_t*>(bytes), len) == ESP_OK;
}
void loadNodePair() {
  ConfigGuard guard;
  Preferences p;
  if (p.begin(CONFIG_NAMESPACE, true)) {
    nodeIsPaired = p.getBool("node_paired", false) && p.getBytesLength("node_mac") == 6;
    if (nodeIsPaired) p.getBytes("node_mac", pairedNodeMac, 6);
    nodeUnpairPending = p.getBool("node_unpair", false);
    committedSession = p.getUInt("pair_session", 0);
    p.end();
  }
  nodeIsPaired = nodeIsPaired && validNodeMac(pairedNodeMac);
  // Migration for installations paired before source selection was persisted:
  // choose their existing node once, while preserving an explicit REMOTE choice.
  if (nodeIsPaired && !nodeUnpairPending && !sensorReferenceConfigured) {
    if (p.begin(CONFIG_NAMESPACE, false)) {
      sensorReferenceConfigured = p.putBool("use_node", true) == 1;
      p.end();
    }
    if (sensorReferenceConfigured) sensorState.selectedNode = true;
  }
  portENTER_CRITICAL(&sensorMux);
  sensorState.resetNode(nodeIsPaired && !nodeUnpairPending);
  portEXIT_CRITICAL(&sensorMux);
  Serial.printf("[NODE_PAIR] tersimpan=%s, pending_unpair=%s\n",
                nodeIsPaired ? "YA" : "TIDAK", nodeUnpairPending ? "YA" : "TIDAK");
  if (nodeIsPaired) printNodeMac("[NODE_PAIR] MAC=", pairedNodeMac);
}
bool storeNodePair(const uint8_t* mac, uint32_t session) {
  ConfigGuard guard;
  if (factoryResetInProgress.load()) return false;
  Preferences p;
  if (!p.begin(CONFIG_NAMESPACE, false)) return false;
  bool ok = p.putBool("node_paired", false) == 1;
  if (ok) ok = p.putBytes("node_mac", mac, 6) == 6;
  if (ok) ok = p.putBool("node_unpair", false) == 1;
  if (ok) ok = p.putUInt("pair_session", session) == 4;
  // A newly approved node becomes the active temperature/RH reference.
  if (ok) ok = p.putBool("use_node", true) == 1;
  if (ok) ok = p.putBool("node_paired", true) == 1;
  p.end();
  if (ok) sensorReferenceConfigured = true;
  return ok;
}
bool requestNodeUnpair() {
  if (!nodeIsPaired) return false;
  if (!nodeUnpairPending) {
    ConfigGuard guard;
    if (factoryResetInProgress.load()) return false;
    Preferences p;
    if (!p.begin(CONFIG_NAMESPACE, false)) return false;
    const bool ok = p.putBool("node_unpair", true) == 1;
    p.end();
    if (!ok) return false;
  }
  nodeUnpairPending = true;
  pairedNodeContactSeen.store(false);
  pairingOpen = false; candidateApproved = false;
  portENTER_CRITICAL(&sensorMux);
  sensorState.resetNode(false);
  portEXIT_CRITICAL(&sensorMux);
  // Keep the revoked MAC + pending marker through restart; retry when it wakes.
  PacketHeader request{};
  PacketHeader unpair{}; replyHeader(unpair, MSG_UNPAIR, request);
  sendNodePacket(pairedNodeMac, &unpair, sizeof(unpair));
  return true;
}
bool forgetNodePairForReplacement() {
  ConfigGuard guard;
  if (factoryResetInProgress.load()) return false;
  Preferences p;
  if (!p.begin(CONFIG_NAMESPACE, false)) return false;
  bool ok = p.putBool("node_paired", false) == 1;
  if (ok) ok = p.putBool("node_unpair", false) == 1;
  if (ok) ok = p.putUInt("pair_session", 0) == 4;
  if (ok) ok = p.putBool("use_node", false) == 1;
  if (ok) p.remove("node_mac");
  p.end();
  if (!ok) return false;

  nodeIsPaired = false;
  nodeUnpairPending = false;
  pairedNodeContactSeen.store(false);
  committedSession = 0;
  sensorReferenceConfigured = true;
  memset(pairedNodeMac, 0, sizeof(pairedNodeMac));
  portENTER_CRITICAL(&sensorMux);
  sensorState.resetNode(false);
  sensorState.selectedNode = false;
  portEXIT_CRITICAL(&sensorMux);
  flagNodeBatteryUpdate.store(true);
  Serial.println("[NODE_PAIR] pasangan lama dilupakan untuk pairing pengganti.");
  return true;
}
size_t expectedNodePacketSize(uint8_t type) {
  switch (type) {
    case MSG_DISCOVERY_REQ: return sizeof(DiscoveryReqPacket);
    case MSG_PAIR_ACCEPT: return sizeof(PairAcceptPacket);
    case MSG_HELLO: return sizeof(PacketHeader);
    case MSG_TELEMETRY: return sizeof(TelemetryPacket);
    case MSG_BATTERY: return sizeof(BatteryPacket);
    default: return 0;
  }
}
void OnDataRecv(const esp_now_recv_info_t* info, const uint8_t* bytes, int len) {
  if (!info || !bytes || !nodeQueue) return;
  nodeRxFrames.fetch_add(1);
  if (len < int(sizeof(PacketHeader)) || len > 64) { nodeRxBadPacket.fetch_add(1); return; }
  PacketHeader header{};
  memcpy(&header, bytes, sizeof(header));
  if (header.magic != AACS_MAGIC || header.version != PROTOCOL_VERSION ||
      expectedNodePacketSize(header.msg_type) != size_t(len) || !validNodeMac(info->src_addr)) {
    nodeRxBadPacket.fetch_add(1); return;
  }
  if (header.msg_type == MSG_TELEMETRY && bytes[offsetof(TelemetryPacket, sensor_valid)] > 1) {
    nodeRxBadPacket.fetch_add(1); return;
  }
  if (header.msg_type == MSG_BATTERY && bytes[offsetof(BatteryPacket, battery_valid)] > 1) {
    nodeRxBadPacket.fetch_add(1); return;
  }
  NodeEvent event;
  memcpy(event.mac, info->src_addr, 6); memcpy(event.bytes, bytes, len);
  event.len = static_cast<uint8_t>(len); event.receivedAt = millis();
  if (xQueueSend(nodeQueue, &event, 0) != pdTRUE) nodeRxQueueFull.fetch_add(1);
}
bool queueNodeCommand(NodeEventKind kind) {
  if (!radioReady.load() || !nodeQueue) return false;
  NodeEvent event; event.kind = kind;
  return xQueueSend(nodeQueue, &event, 0) == pdTRUE;
}
void sendCandidateOffer() {
  PacketHeader request{}; request.session_id = candidateSession;
  PairOfferPacket offer{};
  replyHeader(offer.header, MSG_PAIR_OFFER, request);
  WiFi.macAddress(offer.remote_mac);
  sendNodePacket(candidateNodeMac, &offer, sizeof(offer));
  lastOfferAt = millis();
}
bool consumeNodeTelemetry(const NodeEvent& event, const PacketHeader& header) {
  TelemetryPacket packet{}; memcpy(&packet, event.bytes, sizeof(packet));
  const bool valid = packet.sensor_valid && isfinite(packet.temp_raw) &&
                     packet.temp_raw >= -40 && packet.temp_raw <= 125 &&
                     aacs::validValues(packet.temp_corrected, packet.humidity);
  portENTER_CRITICAL(&sensorMux);
  const bool accepted = sensorState.acceptNode(header.session_id, header.seq_num,
      packet.temp_corrected, packet.humidity, valid, event.receivedAt);
  portEXIT_CRITICAL(&sensorMux);
  AckPacket ack{}; replyHeader(ack.header, MSG_ACK, header);
  ack.ack_seq_num = header.seq_num; ack.ack_msg_type = MSG_TELEMETRY;
  if (!accepted) {
    nodeRxDuplicate.fetch_add(1);
    // A duplicate means the original payload was already consumed but its ACK
    // may have been lost. ACK it again so the node does not re-pair needlessly.
    sendNodePacket(event.mac, &ack, sizeof(ack));
    return false;
  }
  nodeRxAccepted.fetch_add(1);
  if (!valid) nodeRxInvalid.fetch_add(1);
  Serial.printf("[NODE_RX] %s | T=%.2f RH=%.2f raw=%.2f seq=%lu session=%lu\n",
                valid ? "DITERIMA" : "SENSOR_INVALID", packet.temp_corrected,
                packet.humidity, packet.temp_raw, static_cast<unsigned long>(header.seq_num),
                static_cast<unsigned long>(header.session_id));
  sendNodePacket(event.mac, &ack, sizeof(ack));
  return true;
}
void consumeNodeBattery(const NodeEvent& event, const PacketHeader& header) {
  BatteryPacket packet{}; memcpy(&packet, event.bytes, sizeof(packet));
  portENTER_CRITICAL(&sensorMux);
  const bool accepted = sensorState.acceptBattery(header.session_id, header.seq_num,
      packet.battery_mv, packet.battery_percent, packet.battery_valid, event.receivedAt);
  const aacs::NodeBatteryReading reading = sensorState.battery;
  portEXIT_CRITICAL(&sensorMux);
  if (!accepted) { nodeRxDuplicate.fetch_add(1); return; }
  flagNodeBatteryUpdate.store(true); // Publish even when the fresh reading is explicitly invalid.
  if (reading.valid)
    Serial.printf("[NODE_BATTERY] %u mV | %u%%\n", reading.millivolts, reading.percent);
  else
    Serial.println("[NODE_BATTERY] tidak tersedia; periksa divider/konfigurasi.");
  // This must never refresh the temperature timestamp or sequence.
  AckPacket ack{}; replyHeader(ack.header, MSG_ACK, header);
  ack.ack_seq_num = header.seq_num; ack.ack_msg_type = MSG_BATTERY;
  sendNodePacket(event.mac, &ack, sizeof(ack));
}
bool activateNodePair(const uint8_t* mac, uint32_t session) {
  if (!storeNodePair(mac, session)) {
    Serial.println("[NODE_PAIR] NVS gagal; pasangan tidak diaktifkan."); return false;
  }
  memcpy(pairedNodeMac, mac, 6);
  nodeIsPaired = true; nodeUnpairPending = false; committedSession = session;
  pairedNodeContactSeen.store(false);
  portENTER_CRITICAL(&sensorMux);
  sensorState.resetNode(true);
  sensorState.selectedNode = true;
  portEXIT_CRITICAL(&sensorMux);
  pairingOpen = false; candidateApproved = false;
  flagPairingSuccess.store(true);
  printNodeMac("[NODE_PAIR] AKTIF MAC=", mac);
  return true;
}
bool recordCandidate(const NodeEvent& event, const PacketHeader& header, bool alreadyPaired) {
  if (!pairingOpen || (candidatePresent && memcmp(candidateNodeMac, event.mac, 6))) return false;
  const bool changed = !candidatePresent || candidateSession != header.session_id ||
                       candidateAlreadyPaired != alreadyPaired;
  if (changed) { candidateApproved = false; candidateHasTelemetry = false; }
  candidatePresent = true; candidateSession = header.session_id;
  candidateAlreadyPaired = alreadyPaired;
  memcpy(candidateNodeMac, event.mac, 6);
  if (alreadyPaired && header.msg_type == MSG_TELEMETRY) {
    candidateTelemetry = event; candidateHasTelemetry = true;
  }
  if (changed) {
    portENTER_CRITICAL(&sensorMux);
    memcpy(candidateNoticeMac, event.mac, 6);
    portEXIT_CRITICAL(&sensorMux);
    flagNewCandidate.store(true);
    printNodeMac(alreadyPaired ? "[NODE_PAIR] node lama ditemukan, APPROVE dari aplikasi: " : "[NODE_PAIR] kandidat: ", event.mac);
  }
  return true;
}
void processNodeEvent(const NodeEvent& event) {
  const uint32_t now = millis();
  if (pairingOpen && uint32_t(now-pairingOpenedAt) >= 60000) {
    pairingOpen = false; candidateApproved = false; candidatePresent = false;
    Serial.println("Jendela pairing ditutup otomatis.");
  }
  if (event.kind != NodeEventKind::PACKET) {
    if (event.kind == NodeEventKind::OPEN || event.kind == NodeEventKind::REPLACE) {
      if (event.kind == NodeEventKind::REPLACE && !forgetNodePairForReplacement()) {
        Serial.println("[NODE_PAIR] gagal melupakan pasangan lama.");
        return;
      }
      if (nodeIsPaired && !nodeUnpairPending) {
        Serial.println("Node sudah dipasangkan. Unpair dahulu untuk mengganti node."); return;
      }
      if (candidatePresent && (!nodeIsPaired || memcmp(candidateNodeMac, pairedNodeMac, 6)))
        esp_now_del_peer(candidateNodeMac);
      pairingOpen = true; pairingOpenedAt = now;
      candidatePresent = false; candidateApproved = false; candidateHasTelemetry = false;
      candidateAlreadyPaired = false;
      Serial.println("Jendela pairing dibuka 60 detik.");
    } else if (event.kind == NodeEventKind::APPROVE && pairingOpen && candidatePresent) {
      if (candidateAlreadyPaired) {
        // Legacy node still sends to this remote, but old firmware stored no MAC.
        // Restore ONLY after explicit app approval of the observed candidate.
        if (!activateNodePair(candidateNodeMac, candidateSession)) return;
        Serial.println("[NODE_PAIR] pasangan lama dipulihkan; node tidak perlu direset.");
        if (candidateHasTelemetry && uint32_t(now-candidateTelemetry.receivedAt) < aacs::NODE_STALE_TIMEOUT_MS) {
          PacketHeader h{}; memcpy(&h, candidateTelemetry.bytes, sizeof(h));
          consumeNodeTelemetry(candidateTelemetry, h);
        }
        return;
      }
      candidateApproved = true; sendCandidateOffer();
      Serial.println("Kandidat disetujui aplikasi; menunggu ACCEPT.");
    } else if (event.kind == NodeEventKind::REJECT) {
      pairingOpen = false; candidateApproved = false;
    } else if (event.kind == NodeEventKind::UNPAIR) {
      Serial.println(requestNodeUnpair() ? "UNPAIR disimpan; dikirim ulang saat node aktif." : "UNPAIR gagal/tidak ada pasangan.");
    } else if (event.kind == NodeEventKind::APPROVE) {
      Serial.println("[NODE_PAIR] APPROVE ditolak: buka pairing dan tunggu kandidat dahulu.");
    }
    return;
  }
  PacketHeader header{}; memcpy(&header, event.bytes, sizeof(header));
  const bool fromPaired = nodeIsPaired && memcmp(event.mac, pairedNodeMac, 6) == 0;
  if (fromPaired && !nodeUnpairPending &&
      (header.msg_type == MSG_HELLO || header.msg_type == MSG_TELEMETRY ||
       header.msg_type == MSG_BATTERY)) {
    lastPairedNodeContactAt.store(event.receivedAt);
    pairedNodeContactSeen.store(true);
  }
  if (fromPaired && nodeUnpairPending &&
      (header.msg_type == MSG_HELLO || header.msg_type == MSG_TELEMETRY || header.msg_type == MSG_BATTERY)) {
    PacketHeader unpair{}; replyHeader(unpair, MSG_UNPAIR, header);
    sendNodePacket(event.mac, &unpair, sizeof(unpair));
    return;
  }
  if (!fromPaired && (header.msg_type == MSG_HELLO || header.msg_type == MSG_TELEMETRY)) {
    if (nodeIsPaired && !nodeUnpairPending) {
      nodeRxWrongMac.fetch_add(1);
      printNodeMac("[NODE_DROP] MAC berbeda dari pasangan: ", event.mac);
    } else {
      nodeRxUnpaired.fetch_add(1);
      if (!recordCandidate(event, header, true))
        Serial.println("[NODE_DROP] MAC pasangan belum tersimpan. OPEN lalu APPROVE dari aplikasi untuk memulihkan.");
    }
    return;
  }
  if (header.msg_type == MSG_DISCOVERY_REQ && (pairingOpen || (fromPaired && !nodeUnpairPending))) {
    DiscoveryReqPacket request{}; memcpy(&request, event.bytes, sizeof(request));
    if (memcmp(request.node_mac, event.mac, 6)) return;
    const bool automaticRepair = fromPaired && !nodeUnpairPending;
    if (automaticRepair && !pairingOpen) {
      pairingOpen = true;
      pairingOpenedAt = now;
      candidatePresent = false;
      candidateApproved = false;
      candidateHasTelemetry = false;
      candidateAlreadyPaired = false;
    }
    if (!recordCandidate(event, header, false)) return;
    if (automaticRepair) candidateApproved = true;
    DiscoveryRespPacket response{};
    replyHeader(response.header, MSG_DISCOVERY_RESP, header);
    WiFi.macAddress(response.remote_mac);
    const String room = savedRoomId.substring(0, sizeof(response.room_id)-1);
    memcpy(response.room_id, room.c_str(), room.length());
    response.channel = WiFi.channel();
    sendNodePacket(event.mac, &response, sizeof(response));
    if (automaticRepair) {
      sendCandidateOffer();
      Serial.println("[NODE_REPAIR] Node tersimpan masuk discovery; pairing ulang otomatis.");
    }
  } else if (header.msg_type == MSG_PAIR_ACCEPT) {
    PairAcceptPacket accept{}; memcpy(&accept, event.bytes, sizeof(accept));
    if (memcmp(accept.node_mac, event.mac, 6)) return;
    // Resend COMMIT for a lost response, including after a remote restart.
    bool permitted = fromPaired && !nodeUnpairPending && header.session_id == committedSession;
    if (!permitted && pairingOpen && candidatePresent && candidateApproved &&
        memcmp(candidateNodeMac, event.mac, 6) == 0 && header.session_id == candidateSession) {
      if (!activateNodePair(event.mac, header.session_id)) return;
      permitted = true;
    }
    if (!permitted) return; // Never commit a node before app approval.
    PairCommitPacket commit{}; replyHeader(commit.header, MSG_PAIR_COMMIT, header);
    sendNodePacket(event.mac, &commit, sizeof(commit));
    pairingOpen = false; candidateApproved = false;
  } else if (fromPaired && !nodeUnpairPending && header.msg_type == MSG_HELLO) {
    AckPacket ack{}; replyHeader(ack.header, MSG_ACK, header);
    ack.ack_seq_num = header.seq_num; ack.ack_msg_type = MSG_HELLO;
    sendNodePacket(event.mac, &ack, sizeof(ack));
    // HELLO confirms radio contact, not a new temperature measurement.
  } else if (fromPaired && !nodeUnpairPending && header.msg_type == MSG_TELEMETRY) {
    consumeNodeTelemetry(event, header);
  } else if (fromPaired && !nodeUnpairPending && header.msg_type == MSG_BATTERY) {
    consumeNodeBattery(event, header);
  }
}
void nodeRadioTask(void*) {
  if (nodeIsPaired) ensureNodePeer(pairedNodeMac);
  for (;;) {
    if (factoryResetInProgress.load()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    NodeEvent event;
    if (xQueueReceive(nodeQueue, &event, pdMS_TO_TICKS(10)) == pdTRUE) processNodeEvent(event);
    if (pairingOpen && uint32_t(millis()-pairingOpenedAt) >= 60000) {
      pairingOpen = false; candidateApproved = false; candidatePresent = false;
    }
    if (pairingOpen && candidateApproved && uint32_t(millis()-lastOfferAt) >= 700)
      sendCandidateOffer();
  }
}
