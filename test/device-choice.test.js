const test = require("node:test");
const assert = require("node:assert/strict");
const { visibleDevices, chooseSelection, nameFromId, findVirtualCable, isFeedbackLoop } =
  require("../electron/device-choice");

const W = "Windows WASAPI|";

test("visibleDevices keeps only the preferred host API", () => {
  const list = [
    { id: "MME|External Microphone (Synaptics ", preferred: false },
    { id: W + "External Microphone (Synaptics HD Audio)", preferred: true },
  ];
  assert.deepEqual(visibleDevices(list).map((d) => d.id), [W + "External Microphone (Synaptics HD Audio)"]);
  /* No preferred flag anywhere (e.g. older addon / other OS): show all. */
  assert.equal(visibleDevices([{ id: "a" }, { id: "b" }]).length, 2);
  assert.deepEqual(visibleDevices(undefined), []);
});

test("chooseSelection restores a saved, connected device", () => {
  const ids = [W + "Mic A", W + "Mic B"];
  assert.deepEqual(chooseSelection(ids, W + "Mic B", null), { value: W + "Mic B", missing: false });
});

test("chooseSelection reports a saved device that is unplugged", () => {
  assert.deepEqual(chooseSelection([W + "Mic A"], W + "USB Headset", null), { value: "", missing: true });
});

test("chooseSelection uses the suggestion only when nothing was ever chosen", () => {
  const ids = [W + "Speakers", W + "CABLE Input (VB-Audio Virtual Cable)"];
  const cable = W + "CABLE Input (VB-Audio Virtual Cable)";
  assert.equal(chooseSelection(ids, null, cable).value, cable);
  assert.equal(chooseSelection(ids, "", cable).value, "", "explicit default wins over the suggestion");
  assert.equal(chooseSelection(ids, "mute", cable).value, "mute");
  assert.equal(chooseSelection(ids, null, W + "missing").value, "");
});

test("nameFromId strips the host API", () => {
  assert.equal(nameFromId(W + "Speakers (Synaptics HD Audio)"), "Speakers (Synaptics HD Audio)");
  assert.equal(nameFromId("plain"), "plain");
  assert.equal(nameFromId(null), "");
});

const dev = (name, hostApi = "Windows WASAPI") => ({ id: hostApi + "|" + name, name, hostApi });

test("findVirtualCable pairs VB-Cable's playback end with its recording end", () => {
  const outputs = [dev("Speakers (Synaptics HD Audio)"), dev("CABLE Input (VB-Audio Virtual Cable)")];
  const inputs = [dev("External Microphone (Synaptics HD Audio)"), dev("CABLE Output (VB-Audio Virtual Cable)")];
  const c = findVirtualCable(inputs, outputs);
  assert.equal(c.outputName, "CABLE Input (VB-Audio Virtual Cable)");
  assert.equal(c.micName, "CABLE Output (VB-Audio Virtual Cable)", "other apps select the Output end");
  assert.equal(c.outputId, W + "CABLE Input (VB-Audio Virtual Cable)");
});

test("findVirtualCable handles CABLE-A/B, VoiceMeeter and BlackHole", () => {
  assert.equal(findVirtualCable([dev("CABLE-A Output (VB-Audio Cable A)")], [dev("CABLE-A Input (VB-Audio Cable A)")]).micName,
    "CABLE-A Output (VB-Audio Cable A)");
  assert.equal(findVirtualCable([dev("VoiceMeeter Output (VB-Audio VoiceMeeter VAIO)")],
    [dev("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)")]).micName, "VoiceMeeter Output (VB-Audio VoiceMeeter VAIO)");
  assert.equal(findVirtualCable([dev("BlackHole 2ch", "Core Audio")], [dev("BlackHole 2ch", "Core Audio")]).micName,
    "BlackHole 2ch");
});

test("findVirtualCable needs both ends on the same host API", () => {
  assert.equal(findVirtualCable([dev("CABLE Output (VB-Audio Virtual Cable)", "MME")],
    [dev("CABLE Input (VB-Audio Virtual Cable)")]), null);
  assert.equal(findVirtualCable([], [dev("CABLE Input (VB-Audio Virtual Cable)")]), null, "half-installed cable");
  assert.equal(findVirtualCable([dev("Mic")], [dev("Speakers")]), null);
});

test("isFeedbackLoop flags recording from the cable the app writes into", () => {
  const c = findVirtualCable([dev("CABLE Output (VB-Audio Virtual Cable)")], [dev("CABLE Input (VB-Audio Virtual Cable)")]);
  assert.equal(isFeedbackLoop(c.micId, c.outputId, c), true);
  assert.equal(isFeedbackLoop(W + "External Microphone (Synaptics HD Audio)", c.outputId, c), false);
  assert.equal(isFeedbackLoop(c.micId, "mute", c), false);
  assert.equal(isFeedbackLoop(c.micId, c.outputId, null), false);
});
