const test = require("node:test");
const assert = require("node:assert/strict");
const { visibleDevices, chooseSelection, nameFromId } = require("../electron/device-choice");

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
