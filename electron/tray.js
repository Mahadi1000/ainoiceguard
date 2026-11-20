/**
 * Ainoiceguard - System Tray Management
 *
 * Creates a tray icon with context menu for quick ON/OFF control.
 * Clicking the tray icon toggles the main window visibility,
 * positioned near the tray (bottom-right on Windows).
 */

const { Tray, Menu, nativeImage, app, screen } = require("electron");
const path = require("path");

let tray = null;
let mainWindowRef = null;

/**
 * Create the system tray icon and menu.
 * @param {BrowserWindow} mainWindow
 */
function createTray(mainWindow) {
  mainWindowRef = mainWindow;

  tray = new Tray(trayIcon(false));
  tray.setToolTip("Ainoiceguard - Noise Cancellation");

  updateTrayMenu(false);

  /* Click tray icon -> toggle window visibility near the tray. */
  tray.on("click", () => {
    if (!mainWindowRef) return;
    if (mainWindowRef.isVisible()) {
      mainWindowRef.hide();
    } else {
      positionWindowNearTray();
      mainWindowRef.show();
      mainWindowRef.focus();
    }
  });
}

/**
 * Update tray context menu based on running state.
 * @param {boolean} isRunning
 */
function updateTrayMenu(isRunning) {
  if (!tray) return;

  const menu = Menu.buildFromTemplate([
    {
      label: "Ainoiceguard",
      enabled: false,
    },
    { type: "separator" },
    {
      label: isRunning ? "Noise Cancellation: ON" : "Noise Cancellation: OFF",
      enabled: false,
    },
    {
      label: "Show Window",
      click: () => {
        if (mainWindowRef) {
          positionWindowNearTray();
          mainWindowRef.show();
          mainWindowRef.focus();
        }
      },
    },
    { type: "separator" },
    {
      label: "Quit",
      click: () => {
        app.isQuitting = true;
        app.quit();
      },
    },
  ]);

  tray.setContextMenu(menu);

  /* Green shield while running, grey while idle. */
  tray.setImage(trayIcon(isRunning));
}

/**
 * Position the window near the system tray (bottom-right corner on Windows).
 */
function positionWindowNearTray() {
  if (!mainWindowRef || !tray) return;

  const trayBounds = tray.getBounds();
  const windowBounds = mainWindowRef.getBounds();
  const display = screen.getDisplayNearestPoint({
    x: trayBounds.x,
    y: trayBounds.y,
  });
  const workArea = display.workArea;

  /* Position above the tray icon, right-aligned. */
  let x = Math.round(
    trayBounds.x - windowBounds.width / 2 + trayBounds.width / 2,
  );
  let y = Math.round(workArea.y + workArea.height - windowBounds.height - 8);

  /* Clamp to work area. */
  x = Math.max(
    workArea.x,
    Math.min(x, workArea.x + workArea.width - windowBounds.width),
  );
  y = Math.max(workArea.y, y);

  mainWindowRef.setPosition(x, y);
}

/**
 * Tray icon: the shield glyph from electron/assets (tray-on/tray-off.png;
 * Electron picks the @2x file on high-DPI screens). Made by tools/make-icons.js.
 * Falls back to a plain dot if the file is missing.
 * @param {boolean} isRunning
 */
function trayIcon(isRunning) {
  const file = path.join(__dirname, "assets", isRunning ? "tray-on.png" : "tray-off.png");
  const img = nativeImage.createFromPath(file);
  return img.isEmpty() ? nativeImage.createFromDataURL(dotDataURL(isRunning ? "#00d68f" : "#9a9ab4")) : img;
}

/** 16x16 filled circle, used only if the tray PNGs are missing. */
function dotDataURL(color) {
  const size = 16;
  const px = Buffer.alloc(size * size * 4);
  const r = parseInt(color.slice(1, 3), 16);
  const g = parseInt(color.slice(3, 5), 16);
  const b = parseInt(color.slice(5, 7), 16);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const i = (y * size + x) * 4;
      /* nativeImage buffers are BGRA */
      if (Math.hypot(x - 7.5, y - 7.5) <= 6) px.set([b, g, r, 255], i);
    }
  }
  return nativeImage.createFromBuffer(px, { width: size, height: size }).toDataURL();
}

function destroyTray() {
  if (tray) {
    tray.destroy();
    tray = null;
  }
}

module.exports = { createTray, destroyTray, updateTrayMenu };
