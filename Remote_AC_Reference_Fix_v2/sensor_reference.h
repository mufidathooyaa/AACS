#pragma once
#include <stdint.h>
#include <math.h>

namespace aacs {
constexpr uint32_t NODE_STALE_TIMEOUT_MS = 20000;
constexpr uint32_t REMOTE_STALE_TIMEOUT_MS = 20000;
enum class Source : uint8_t { REMOTE, NODE, NONE };
enum class Issue : uint8_t { NONE, PAIR_REQUIRED, WAIT_NODE, SENSOR_INVALID, NODE_STALE, REMOTE_INVALID };
inline const char* issueName(Issue issue) {
  switch (issue) {
    case Issue::PAIR_REQUIRED: return "PAIR NODE";
    case Issue::WAIT_NODE: return "WAIT NODE";
    case Issue::SENSOR_INVALID: return "NODE INVALID";
    case Issue::NODE_STALE: return "NODE STALE";
    case Issue::REMOTE_INVALID: return "REMOTE INVALID";
    default: return "OK";
  }
}
struct Reading {
  float temperature = NAN;
  float humidity = NAN;
  bool valid = false;
  uint32_t updatedAt = 0;
};
inline bool validValues(float t, float h) {
  return isfinite(t) && isfinite(h) && t >= -40 && t <= 125 && h >= 0 && h <= 100;
}
inline bool fresh(const Reading& r, uint32_t now, uint32_t timeout) {
  return r.valid && validValues(r.temperature, r.humidity) && uint32_t(now-r.updatedAt) < timeout;
}
struct ActiveReading {
  Reading reading;
  Source source = Source::NONE;
  bool selectedNode = false;
  bool fallback = false;
  Issue issue = Issue::NONE;
};
struct NodeBatteryReading {
  uint16_t millivolts = 0;
  uint8_t percent = 255;
  bool valid = false;
  uint32_t updatedAt = 0;
};
struct SensorReferenceState {
  Reading remote;
  Reading node;
  bool selectedNode = false;
  bool nodePaired = false;
  bool nodeSequenceSeen = false;
  uint32_t nodeSession = 0;
  uint32_t nodeSequence = 0;
  NodeBatteryReading battery;
  bool batterySequenceSeen = false;
  uint32_t batterySession = 0;
  uint32_t batterySequence = 0;

  void setRemote(float t, float h, bool valid, uint32_t now) {
    remote.temperature = t; remote.humidity = h;
    remote.valid = valid && validValues(t, h); remote.updatedAt = now;
  }
  constexpr void resetNode(bool paired) {
    node = Reading{}; nodePaired = paired; nodeSequenceSeen = false;
    battery = NodeBatteryReading{}; batterySequenceSeen = false;
  }
  constexpr bool acceptBattery(uint32_t session, uint32_t seq, uint16_t mv, uint8_t percent,
                     bool valid, uint32_t now) {
    if (!nodePaired) return false;
    const uint32_t distance = seq-batterySequence;
    if (batterySequenceSeen && session == batterySession && (distance == 0 || distance >= 0x80000000UL))
      return false;
    batterySequenceSeen = true; batterySession = session; batterySequence = seq;
    battery.valid = valid && mv >= 2500 && mv <= 4350 && percent <= 100;
    battery.millivolts = battery.valid ? mv : 0;
    battery.percent = battery.valid ? percent : 255;
    battery.updatedAt = now;
    return true;
  }
  constexpr NodeBatteryReading selectBattery(uint32_t now) const {
    if (!nodePaired || !battery.valid || uint32_t(now-battery.updatedAt) >= NODE_STALE_TIMEOUT_MS)
      return NodeBatteryReading{};
    return battery;
  }
  bool acceptNode(uint32_t session, uint32_t seq, float t, float h, bool valid, uint32_t now) {
    if (!nodePaired) return false;
    // Duplicate/older packets in the same boot must not extend freshness.
    const uint32_t distance = seq-nodeSequence;
    if (nodeSequenceSeen && session == nodeSession && (distance == 0 || distance >= 0x80000000UL))
      return false;
    nodeSequenceSeen = true; nodeSession = session; nodeSequence = seq;
    node.temperature = t; node.humidity = h;
    node.valid = valid && validValues(t, h); node.updatedAt = now;
    return true;
  }
  ActiveReading select(uint32_t now, bool allowRemoteFallback = false) const {
    ActiveReading out;
    out.selectedNode = selectedNode;
    if (selectedNode) {
      if (!nodePaired) out.issue = Issue::PAIR_REQUIRED;
      else if (!nodeSequenceSeen) out.issue = Issue::WAIT_NODE;
      else if (!node.valid || !validValues(node.temperature, node.humidity)) out.issue = Issue::SENSOR_INVALID;
      else if (!fresh(node, now, NODE_STALE_TIMEOUT_MS)) out.issue = Issue::NODE_STALE;
      else {
        out.reading = node; out.source = Source::NODE;
        return out;
      }
      // A label NODE must never silently display/publish the remote values.
      if (!allowRemoteFallback) return out;
    }
    if (fresh(remote, now, REMOTE_STALE_TIMEOUT_MS)) {
      out.reading = remote; out.source = Source::REMOTE; out.fallback = selectedNode;
    } else if (!selectedNode) {
      out.issue = Issue::REMOTE_INVALID;
    }
    // Remote selection never silently uses node data if the remote sensor fails.
    return out;
  }
};
} // namespace aacs
