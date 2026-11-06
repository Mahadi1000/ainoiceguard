/**
 * Device selection by stable identity instead of PortAudio index.
 *
 * PortAudio indices change whenever devices appear or disappear (measured:
 * starting Krisp shifted index 5 from a microphone to "Krisp Speaker"), so a
 * stored index can silently point at the wrong device. A DeviceSpec names a
 * device by host API + name and is resolved against a fresh device list every
 * time streams are opened, including automatic restarts after a device loss.
 *
 * Pure logic (no PortAudio calls) so it can be unit-tested.
 */

#ifndef AINOICEGUARD_DEVICE_SELECT_H
#define AINOICEGUARD_DEVICE_SELECT_H

#include <string>
#include <vector>

namespace ainoiceguard {

/** One device as seen in a PortAudio enumeration. */
struct DeviceEntry {
  int index = -1;
  std::string name;
  std::string hostApi;     /* e.g. "Windows WASAPI", "MME", "ALSA", "Core Audio" */
  int maxInputChannels = 0;
  int maxOutputChannels = 0;
  bool isHostDefaultInput = false;   /* default input of its own host API */
  bool isHostDefaultOutput = false;
};

/** What the caller asked for. */
struct DeviceSpec {
  enum class Kind { Default, ById, ByIndex, None };
  Kind kind = Kind::Default;
  std::string id;  /* "<hostApi>|<name>" (ById) */
  int index = -1;  /* legacy (ByIndex) */

  static DeviceSpec def() { return {}; }
  static DeviceSpec none() { DeviceSpec s; s.kind = Kind::None; return s; }
  static DeviceSpec byId(std::string id) { DeviceSpec s; s.kind = Kind::ById; s.id = std::move(id); return s; }
  static DeviceSpec byIndex(int i) { DeviceSpec s; s.kind = Kind::ByIndex; s.index = i; return s; }
};

inline std::string deviceId(const DeviceEntry& d) { return d.hostApi + "|" + d.name; }

/** Result of resolving a spec. */
struct Resolved {
  int index = -1;            /* -1 = nothing usable */
  bool fellBack = false;     /* requested device missing; using the default instead */
};

/**
 * Resolve a spec for one direction.
 *  - ById:    exact id match (host API + name). The same name on another host
 *             API is deliberately NOT accepted: that is a different latency
 *             and format path. If missing, fall back to the default below and
 *             report fellBack.
 *  - ByIndex: kept for compatibility; must have channels in that direction.
 *  - Default: the default device of `preferredHostApi` if it exists, else the
 *             first host-default found, else the first device with channels.
 *  - None:    -1 (e.g. output muted).
 */
template <typename Device> /* DeviceEntry or a type derived from it */
inline Resolved resolveDevice(const std::vector<Device>& devices, const DeviceSpec& spec,
                              bool input, const std::string& preferredHostApi) {
  auto hasDir = [&](const DeviceEntry& d) {
    return input ? d.maxInputChannels > 0 : d.maxOutputChannels > 0;
  };
  auto isDefault = [&](const DeviceEntry& d) {
    return input ? d.isHostDefaultInput : d.isHostDefaultOutput;
  };
  auto defaultIndex = [&]() {
    for (const auto& d : devices)
      if (hasDir(d) && isDefault(d) && d.hostApi == preferredHostApi) return d.index;
    for (const auto& d : devices)
      if (hasDir(d) && isDefault(d)) return d.index;
    for (const auto& d : devices)
      if (hasDir(d)) return d.index;
    return -1;
  };

  Resolved r;
  switch (spec.kind) {
    case DeviceSpec::Kind::None:
      return r;
    case DeviceSpec::Kind::Default:
      r.index = defaultIndex();
      return r;
    case DeviceSpec::Kind::ByIndex:
      for (const auto& d : devices)
        if (d.index == spec.index && hasDir(d)) { r.index = d.index; return r; }
      break;
    case DeviceSpec::Kind::ById:
      for (const auto& d : devices)
        if (hasDir(d) && deviceId(d) == spec.id) { r.index = d.index; return r; }
      break;
  }
  /* Requested device not present (unplugged, renamed, index shifted). */
  r.index = defaultIndex();
  r.fellBack = r.index >= 0;
  return r;
}

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_DEVICE_SELECT_H
