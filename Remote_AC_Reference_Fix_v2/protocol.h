#ifndef AACS_PROTOCOL_H
#define AACS_PROTOCOL_H

#include <stdint.h>

// Identifier Protokol (Magic Bytes) untuk memastikan paket bukan dari alat lain
const uint32_t AACS_MAGIC = 0xAA00CC55;
const uint8_t PROTOCOL_VERSION = 1;

// Tipe Pesan ESP-NOW
enum MessageType : uint8_t {
    MSG_DISCOVERY_REQ = 1,   // Node -> Broadcast
    MSG_DISCOVERY_RESP = 2,  // Remote -> Node (Unicast)
    MSG_PAIR_OFFER = 3,      // Remote -> Node (Setelah di-approve Backend)
    MSG_PAIR_ACCEPT = 4,     // Node -> Remote
    MSG_PAIR_COMMIT = 5,     // Remote -> Node (Simpan permanen)
    MSG_HELLO = 6,           // Node -> Remote (Reconnect/Bootup)
    MSG_TELEMETRY = 7,       // Node -> Remote (Data SHT31)
    MSG_ACK = 8,             // Remote -> Node (General ACK)
    MSG_UNPAIR = 9,          // Remote -> Node (Hapus pasangan)
    MSG_BATTERY = 10         // Node -> Remote (LiPo, independent of SHT31 validity)
};

// =========================================================================
// STRUKTUR DATA PAKET ESP-NOW
// (Gunakan packed agar ukuran sama di berbagai arsitektur)
// =========================================================================

#pragma pack(push, 1)

// Header dasar yang ada di SEMUA paket
struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t msg_type;
    uint32_t session_id; // Nonce sesi / Boot ID
    uint32_t seq_num;    // Nomor urut paket
};

// MSG_DISCOVERY_REQ (Node -> Broadcast)
struct DiscoveryReqPacket {
    PacketHeader header;
    uint8_t node_mac[6]; // Identitas teknis Node
};

// MSG_DISCOVERY_RESP (Remote -> Node)
struct DiscoveryRespPacket {
    PacketHeader header;
    uint8_t remote_mac[6];
    char room_id[16];    // Null-terminated string
    uint8_t channel;     // Channel WiFi Remote (AP)
};

// MSG_PAIR_OFFER (Remote -> Node)
struct PairOfferPacket {
    PacketHeader header;
    uint8_t remote_mac[6];
};

// MSG_PAIR_ACCEPT (Node -> Remote)
struct PairAcceptPacket {
    PacketHeader header;
    uint8_t node_mac[6];
};

// MSG_PAIR_COMMIT (Remote -> Node)
struct PairCommitPacket {
    PacketHeader header;
};

// MSG_TELEMETRY (Node -> Remote)
struct TelemetryPacket {
    PacketHeader header;
    float temp_raw;
    float temp_corrected;
    float humidity;
    bool sensor_valid;
};

// Separate packet preserves the existing 27-byte temperature telemetry format.
struct BatteryPacket {
    PacketHeader header;
    uint16_t battery_mv;       // 0 when unavailable.
    uint8_t battery_percent;   // 0..100; 255 when unavailable.
    uint8_t battery_valid;     // 0 or 1, explicitly checked by receiver.
};

// MSG_ACK (Remote -> Node)
struct AckPacket {
    PacketHeader header;
    uint32_t ack_seq_num; // Meng-ACK nomor urut spesifik
    uint8_t ack_msg_type; // Tipe pesan yang di-ACK
};

#pragma pack(pop)

// Pastikan paket tidak melebihi batas ESP-NOW (250 bytes)
static_assert(sizeof(TelemetryPacket) <= 250, "TelemetryPacket terlalu besar!");
static_assert(sizeof(TelemetryPacket) == 27, "Legacy telemetry wire format changed");
static_assert(sizeof(BatteryPacket) == 18, "Battery wire format changed");
static_assert(sizeof(DiscoveryRespPacket) <= 250, "DiscoveryRespPacket terlalu besar!");

#endif // AACS_PROTOCOL_H
