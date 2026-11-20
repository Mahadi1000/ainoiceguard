/**
 * Renders the app icon SVGs in electron/assets/ to PNG + ICO.
 *
 *   ./node_modules/.bin/electron tools/make-icons.js
 *
 * Outputs (electron/assets/):
 *   icon.png          512 px, installer/Linux/macOS icon (electron-builder)
 *   icon.ico          16..256 px, Windows exe + installer icon
 *   icon-256.png      window icon (BrowserWindow)
 *   tray-on.png  (+@2x)   tray icon while noise cancellation runs
 *   tray-off.png (+@2x)   tray icon while idle
 *
 * Sizes <= 32 px use icon-small.svg (fewer details stay readable). Each size
 * is drawn on a canvas at its exact pixel size, so nothing is downscaled.
 */
const { app, BrowserWindow } = require("electron");
const fs = require("fs");
const path = require("path");

const DIR = path.join(__dirname, "..", "electron", "assets");
const read = (f) => fs.readFileSync(path.join(DIR, f), "utf8");

const TRAY_ON = "#00d68f";
const TRAY_OFF = "#9a9ab4";

/** Pack PNG images into one .ico (PNG-compressed entries, Vista+). */
function packIco(entries) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2); /* type 1 = icon */
  header.writeUInt16LE(entries.length, 4);
  const dir = Buffer.alloc(16 * entries.length);
  let offset = 6 + dir.length;
  entries.forEach(({ size, png }, i) => {
    const o = i * 16;
    dir.writeUInt8(size >= 256 ? 0 : size, o); /* 0 means 256 */
    dir.writeUInt8(size >= 256 ? 0 : size, o + 1);
    dir.writeUInt8(0, o + 2); /* palette */
    dir.writeUInt8(0, o + 3);
    dir.writeUInt16LE(1, o + 4); /* planes */
    dir.writeUInt16LE(32, o + 6); /* bpp */
    dir.writeUInt32LE(png.length, o + 8);
    dir.writeUInt32LE(offset, o + 12);
    offset += png.length;
  });
  return Buffer.concat([header, dir, ...entries.map((e) => e.png)]);
}

app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false, webPreferences: { offscreen: true } });
  await win.loadURL("data:text/html,<canvas></canvas>");

  /* Draw an SVG string at size x size, return PNG bytes. */
  const render = async (svg, size) => {
    const url = await win.webContents.executeJavaScript(`(async () => {
      const img = new Image();
      img.src = "data:image/svg+xml;base64," + ${JSON.stringify(Buffer.from(svg).toString("base64"))};
      await img.decode();
      const c = document.createElement("canvas");
      c.width = c.height = ${size};
      const g = c.getContext("2d");
      g.imageSmoothingQuality = "high";
      g.drawImage(img, 0, 0, ${size}, ${size});
      return c.toDataURL("image/png");
    })()`);
    return Buffer.from(url.split(",")[1], "base64");
  };

  const big = read("icon.svg");
  const small = read("icon-small.svg");
  const tray = read("tray.svg");
  const pick = (size) => (size <= 32 ? small : big);

  const write = (name, buf) => {
    fs.writeFileSync(path.join(DIR, name), buf);
    console.log(`  ${name}  ${buf.length} bytes`);
  };

  write("icon.png", await render(big, 512));
  write("icon-256.png", await render(big, 256));

  const icoSizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
  const entries = [];
  for (const size of icoSizes) entries.push({ size, png: await render(pick(size), size) });
  write("icon.ico", packIco(entries));

  for (const [name, color] of [["tray-on", TRAY_ON], ["tray-off", TRAY_OFF]]) {
    const svg = tray.replace(/COLOR/g, color);
    write(`${name}.png`, await render(svg, 16));
    write(`${name}@2x.png`, await render(svg, 32));
  }

  app.quit();
});
