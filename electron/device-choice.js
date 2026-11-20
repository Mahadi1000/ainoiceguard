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

  /*
   * Virtual-cable pairing. A virtual cable has two ends:
   *   - a playback device the app writes cleaned audio to, and
   *   - a recording device other apps (Zoom, Discord, Meet) select as mic.
   * For VB-Audio products the playback end is "... Input (...)" and the
   * recording end "... Output (...)" -- the reverse of what users expect,
   * and the previous UI text had it backwards.
   */
  const CABLE_PAIRS = [
    /* [playback-end regex, function: playback name -> recording name] */
    [/^(CABLE(?:-[A-D])? )Input( \(VB-Audio.*\))$/i, (m) => m[1] + "Output" + m[2]],
    [/^(VoiceMeeter(?: Aux| VAIO3)? )Input( \(VB-Audio.*\))$/i, (m) => m[1] + "Output" + m[2]],
    [/^(BlackHole \d+ch)$/i, (m) => m[1]], /* macOS: same name on both ends */
  ];

  /**
   * Find a virtual cable among the devices.
   * @returns {{ outputId, outputName, micId, micName } | null}
   *   outputId: where the app should write; micName: what other apps select.
   */
  function findVirtualCable(inputs, outputs) {
    const ins = Array.isArray(inputs) ? inputs : [];
    for (const o of Array.isArray(outputs) ? outputs : []) {
      for (const [re, toMic] of CABLE_PAIRS) {
        const m = re.exec(o.name || "");
        if (!m) continue;
        const micName = toMic(m);
        const mic = ins.find((i) => i.name === micName && i.hostApi === o.hostApi);
        if (mic) return { outputId: o.id, outputName: o.name, micId: mic.id, micName: mic.name };
      }
    }
    return null;
  }

  /**
   * True if using `inputId` as the app's microphone while writing to
   * `outputId` would feed the app's own output back into itself.
   */
  function isFeedbackLoop(inputId, outputId, cable) {
    return !!cable && inputId === cable.micId && outputId === cable.outputId;
  }

  /**
   * One line for the VB-Cable banner from getCaptureClients(): which apps
   * record from the cable. Duplicate processes (one app, several sessions)
   * are listed once; ".exe" is dropped. null input = unknown -> null.
   * @returns {{ text: string, inUse: boolean } | null}
   */
  function describeCaptureClients(clients, micName) {
    if (!Array.isArray(clients)) return null;
    const label = (c) => (c.process ? c.process.replace(/\.exe$/i, "") : "PID " + c.pid);
    const uniq = (list) => [...new Set(list.map(label))];
    const active = uniq(clients.filter((c) => c.active));
    const idle = uniq(clients.filter((c) => !c.active)).filter((n) => !active.includes(n));
    if (active.length) {
      return { text: "In use by: " + active.join(", "), inUse: true };
    }
    if (idle.length) {
      return { text: "Open in: " + idle.join(", ") + " (not recording right now)", inUse: false };
    }
    return { text: "No app is using " + (micName || "the cable") + " yet.", inUse: false };
  }

  const api = {
    visibleDevices,
    chooseSelection,
    nameFromId,
    findVirtualCable,
    isFeedbackLoop,
    describeCaptureClients,
  };

  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }
  if (globalScope) {
    globalScope.deviceChoice = api;
  }
})(typeof window !== "undefined" ? window : globalThis);
