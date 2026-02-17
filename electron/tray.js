/**
 * NoiseGuard - System Tray Management
 *
 * Creates a tray icon with context menu for quick ON/OFF control.
 * Clicking the tray icon toggles the main window visibility,
 * positioned near the tray (bottom-right on Windows).
 */

const { Tray, Menu, nativeImage, app, screen } = require('electron');
const path = require('path');

let tray = null;
let mainWindowRef = null;

/**
 * Create the system tray icon and menu.
 * @param {BrowserWindow} mainWindow
 */
function createTray(mainWindow) {
  mainWindowRef = mainWindow;

  /*
   * Create a simple 16x16 tray icon.
   * In production, replace with a proper .ico/.png asset.
   * For the starter, we generate a colored square programmatically.
   */
  const icon = nativeImage.createFromDataURL(generateTrayIconDataURL('#00CC88'));

  tray = new Tray(icon);
  tray.setToolTip('NoiseGuard - Noise Cancellation');

  updateTrayMenu(false);

  /* Click tray icon -> toggle window visibility near the tray. */
  tray.on('click', () => {
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
      label: 'NoiseGuard',
      enabled: false,
    },
    { type: 'separator' },
    {
      label: isRunning ? 'Noise Cancellation: ON' : 'Noise Cancellation: OFF',
      enabled: false,
    },
    {
      label: 'Show Window',
      click: () => {
        if (mainWindowRef) {
          positionWindowNearTray();
          mainWindowRef.show();
          mainWindowRef.focus();
        }
      },
    },
    { type: 'separator' },
    {
      label: 'Quit',
      click: () => {
        app.isQuitting = true;
        app.quit();
      },
    },
  ]);

  tray.setContextMenu(menu);

  /* Update icon color based on state. */
  const color = isRunning ? '#00CC88' : '#666666';
  tray.setImage(nativeImage.createFromDataURL(generateTrayIconDataURL(color)));
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
  let x = Math.round(trayBounds.x - windowBounds.width / 2 + trayBounds.width / 2);
  let y = Math.round(workArea.y + workArea.height - windowBounds.height - 8);

  /* Clamp to work area. */
  x = Math.max(workArea.x, Math.min(x, workArea.x + workArea.width - windowBounds.width));
  y = Math.max(workArea.y, y);

  mainWindowRef.setPosition(x, y);
}

/**
 * Generate a simple colored square icon as a data URL.
 * @param {string} color - CSS hex color
 * @returns {string} data URL
 */
function generateTrayIconDataURL(color) {
  /*
   * Create a 16x16 PNG with a colored circle.
   * This is a minimal programmatic icon for the starter.
   * Replace with a proper icon file in production.
   *
   * We use a simple base64 encoded 16x16 image.
   * For now, just use a colored rectangle via nativeImage.
   */
  const size = 16;
  const canvas = Buffer.alloc(size * size * 4);

  /* Parse hex color. */
  const r = parseInt(color.slice(1, 3), 16);
  const g = parseInt(color.slice(3, 5), 16);
  const b = parseInt(color.slice(5, 7), 16);

  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const cx = x - size / 2 + 0.5;
      const cy = y - size / 2 + 0.5;
      const dist = Math.sqrt(cx * cx + cy * cy);
      const idx = (y * size + x) * 4;

      if (dist <= 6) {
        canvas[idx] = r;       /* R */
        canvas[idx + 1] = g;   /* G */
        canvas[idx + 2] = b;   /* B */
        canvas[idx + 3] = 255; /* A */
      } else {
        canvas[idx + 3] = 0;   /* Transparent */
      }
    }
  }

  const img = nativeImage.createFromBuffer(canvas, { width: size, height: size });
  return img.toDataURL();
}

function destroyTray() {
  if (tray) {
    tray.destroy();
    tray = null;
  }
}

module.exports = { createTray, destroyTray, updateTrayMenu };
