/**
 * What to do when the set of audio endpoints changes while the engine runs.
 *
 * Inputs: what the user asked for (DeviceSpec), what is actually open, and a
 * fresh snapshot of active endpoints + the OS default device. Output: keep
 * going, switch back to the preferred device, or follow the new default.
 *
 * Pure logic (no OS calls) so it can be unit-tested; the Windows endpoint
 * watcher (device_watch.h) supplies the snapshot.
 */

#ifndef AINOICEGUARD_DEVICE_POLICY_H
#define AINOICEGUARD_DEVICE_POLICY_H

#include <algorithm>
#include <string>
#include <vector>

#include "device_select.h"

namespace ainoiceguard {

/** Active endpoints of the preferred host API (names as PortAudio shows them). */
struct EndpointSnapshot {
  std::vector<std::string> capture;  /* active recording endpoints */
  std::vector<std::string> render;   /* active playback endpoints */
  std::string defaultCapture;        /* OS default, "" if none */
  std::string defaultRender;
};

enum class Reselect {
  None,              /* keep the current device */
  ReturnToPreferred, /* we fell back; the requested device is back */
  FollowDefault,     /* "System Default" requested and the OS default moved */
};

/** Name part of an id "<hostApi>|<name>" if its host API is `hostApi`, else "". */
inline std::string nameForHost(const std::string& id, const std::string& hostApi) {
  const size_t bar = id.find('|');
  if (bar == std::string::npos || id.compare(0, bar, hostApi) != 0) return "";
  return id.substr(bar + 1);
}

/**
 * Decide for one direction.
 * @param openedName  name of the device currently open ("" if none)
 * @param hostApi     the host API the snapshot describes (e.g. "Windows WASAPI")
 */
inline Reselect decideReselect(const DeviceSpec& spec, const std::string& openedName,
                               const EndpointSnapshot& snap, bool input,
                               const std::string& hostApi) {
  const auto& active = input ? snap.capture : snap.render;
  const std::string& def = input ? snap.defaultCapture : snap.defaultRender;

  switch (spec.kind) {
    case DeviceSpec::Kind::ById: {
      const std::string want = nameForHost(spec.id, hostApi);
      if (want.empty() || want == openedName) return Reselect::None;
      const bool present = std::find(active.begin(), active.end(), want) != active.end();
      return present ? Reselect::ReturnToPreferred : Reselect::None;
    }
    case DeviceSpec::Kind::Default:
      /* Only when there is a new default and it is not what we already use. */
      return (!def.empty() && def != openedName) ? Reselect::FollowDefault : Reselect::None;
    case DeviceSpec::Kind::ByIndex: /* legacy: no identity to return to */
    case DeviceSpec::Kind::None:
      return Reselect::None;
  }
  return Reselect::None;
}

}  // namespace ainoiceguard

#endif  // AINOICEGUARD_DEVICE_POLICY_H
