/**
 * Device-list rules for the renderer (pure; loaded as a classic script in
 * index.html and require()-able in tests, like metrics-utils.js).
 *
 *  - Only devices on the engine's preferred host API are listed (WASAPI on
 *    Windows). The full PortAudio list repeats every device per host API and
 *    MME truncates names ("External Microphone (Synaptics "), which made it
 *    easy to pick the higher-latency path by accident.
 *  - Selections are by stable id. A saved device that is not connected is
 *    reported as missing; the select then shows the default instead, but the
 *    saved choice is kept so it is used again once the device is back.
 */
(function setupDeviceChoice(globalScope) {
  /** Devices to show: preferred host API only, or all if none is marked. */
  function visibleDevices(devices) {
    const list = Array.isArray(devices) ? devices : [];
    const preferred = list.filter((d) => d && d.preferred);
    return preferred.length ? preferred : list;
  }

  /**
   * Decide what a <select> should show.
   * @param {string[]} available  ids present in the list
   * @param {string|null} savedId saved choice: id, "" (default), "mute", or
   *                              null (never chosen)
   * @param {string|null} autoId  suggestion when nothing was ever chosen
   *                              (e.g. VB-Cable for the output)
   * @returns {{ value: string, missing: boolean }}
   *          value: id, "" (default) or "mute"; missing: saved id not present
   */
  function chooseSelection(available, savedId, autoId) {
    const ids = new Set(available || []);
    if (savedId === null || savedId === undefined) {
      return { value: autoId && ids.has(autoId) ? autoId : "", missing: false };
    }
    if (savedId === "" || savedId === "mute") return { value: savedId, missing: false };
    if (ids.has(savedId)) return { value: savedId, missing: false };
    return { value: "", missing: true };
  }

  /** Display name of an id ("<hostApi>|<name>" -> name). */
  function nameFromId(id) {
    if (typeof id !== "string") return "";
    const i = id.indexOf("|");
    return i >= 0 ? id.slice(i + 1) : id;
  }

  const api = { visibleDevices, chooseSelection, nameFromId };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }
  if (globalScope) {
    globalScope.deviceChoice = api;
  }
})(typeof window !== "undefined" ? window : globalThis);
