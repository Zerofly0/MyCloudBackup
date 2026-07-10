const { app, BrowserWindow, dialog, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { spawn } = require('child_process');

const isWin = process.platform === 'win32';
const coreName = isWin ? 'backup_core.exe' : 'backup_core';
const tempRoot = path.join(app.getPath('userData'), 'temp');
const downloadRoot = path.join(app.getPath('userData'), 'downloads');
const configDir = path.join(app.getPath('userData'), 'config');
const configFile = path.join(configDir, 'client_config.json');

function bundledCorePath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'bin', coreName);
  }
  return path.resolve(__dirname, '..', '..', 'build', coreName);
}

function resolveCorePath(savedCorePath) {
  const fallback = bundledCorePath();
  if (savedCorePath && fs.existsSync(savedCorePath)) {
    return savedCorePath;
  }
  return fallback;
}

function ensureDirs() {
  fs.mkdirSync(tempRoot, { recursive: true });
  fs.mkdirSync(downloadRoot, { recursive: true });
  fs.mkdirSync(configDir, { recursive: true });
}

function readConfig() {
  ensureDirs();
  const defaultCorePath = bundledCorePath();
  if (!fs.existsSync(configFile)) {
    return {
      serverUrl: 'http://8.137.100.109:8080',
      maxBackups: 10,
      scheduleInterval: 60,
      lastSourceDir: '',
      lastRestoreDir: '',
      corePath: defaultCorePath
    };
  }
  const saved = JSON.parse(fs.readFileSync(configFile, 'utf8'));
  return {
    serverUrl: 'http://8.137.100.109:8080',
    maxBackups: 10,
    scheduleInterval: 60,
    lastSourceDir: '',
    lastRestoreDir: '',
    ...saved,
    corePath: resolveCorePath(saved.corePath)
  };
}

function saveConfig(config) {
  ensureDirs();
  fs.writeFileSync(configFile, JSON.stringify(config, null, 2));
}

function createWindow() {
  const win = new BrowserWindow({
    width: 1180,
    height: 760,
    minWidth: 980,
    minHeight: 620,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });
  win.loadFile(path.join(__dirname, 'index.html'));
}

app.whenReady().then(() => {
  ensureDirs();
  createWindow();
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

ipcMain.handle('config:load', () => readConfig());
ipcMain.handle('config:save', (_, config) => {
  saveConfig(config);
  return { success: true };
});

ipcMain.handle('dialog:directory', async (_, title) => {
  const result = await dialog.showOpenDialog({ title, properties: ['openDirectory'] });
  return result.canceled ? '' : result.filePaths[0];
});

function runCore(args, onLog) {
  return new Promise((resolve) => {
    const config = readConfig();
    const corePath = resolveCorePath(config.corePath);
    if (!fs.existsSync(corePath)) {
      resolve({
        success: false,
        message: `C++ core program not found: ${corePath}`,
        stdout: '',
        stderr: ''
      });
      return;
    }
    onLog(`C++ core path: ${corePath}\n`);
    const child = spawn(corePath, args, { windowsHide: true });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => {
      const text = chunk.toString();
      stdout += text;
      onLog(text);
    });
    child.stderr.on('data', (chunk) => {
      const text = chunk.toString();
      stderr += text;
      onLog(text);
    });
    child.on('error', (err) => {
      onLog(`C++ core start error: ${err.message}\n`);
      resolve({ success: false, message: err.message, stdout, stderr });
    });
    child.on('close', (code) => {
      const lines = (stdout + '\n' + stderr).split(/\r?\n/).filter(Boolean);
      const jsonLine = [...lines].reverse().find((line) => line.trim().startsWith('{'));
      let parsed = {};
      try { parsed = jsonLine ? JSON.parse(jsonLine) : {}; } catch (_) {}
      if (code !== 0) {
        onLog(`C++ core exited with code: ${code}\n`);
      }
      if (!jsonLine && code !== 0) {
        onLog('C++ core did not return a JSON result.\n');
      }
      resolve({ success: code === 0 && parsed.success !== false, code, stdout, stderr, ...parsed });
    });
  });
}

function sanitizePackageName(name, fallback) {
  let base = String(name || fallback || "").trim();
  if (base.toLowerCase().endsWith(".bak.enc")) {
    base = base.slice(0, -8);
  }
  base = base
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, "_")
    .replace(/\s+/g, "_")
    .replace(/^\.+|\.+$/g, "");
  return base || fallback;
}


function makeUniquePackageName(baseName, existingNames) {
  const used = new Set((existingNames || []).map((name) => String(name).toLowerCase()));
  let candidate = baseName;
  let index = 1;
  while (used.has(`${candidate}.bak.enc`.toLowerCase())) {
    candidate = `${baseName}(${index})`;
    index += 1;
  }
  return candidate;
}


function writeFilterFile(filter) {
  const file = path.join(tempRoot, `filter_${Date.now()}.json`);
  fs.writeFileSync(file, JSON.stringify(filter, null, 2));
  return file;
}

ipcMain.handle('core:backup', async (event, payload) => {
  ensureDirs();
  const timestamp = new Date().toISOString().replace(/[-:T.Z]/g, '').slice(0, 14);
  const packageBaseName = makeUniquePackageName(
    sanitizePackageName(payload.packageName, `backup_${timestamp}`),
    payload.existingNames
  );
  const output = path.join(tempRoot, `${packageBaseName}.bak.enc`);
  const filterFile = writeFilterFile(payload.filter || {});
  return runCore([
    'backup',
    '--source', payload.sourceDir,
    '--output', output,
    '--password', payload.password,
    '--filter', filterFile
  ], (text) => event.sender.send('task:log', text));
});

ipcMain.handle('core:restore', async (event, payload) => {
  return runCore([
    'restore',
    '--input', payload.inputFile,
    '--restore', payload.restoreDir,
    '--password', payload.password,
    '--overwrite', 'replace'
  ], (text) => event.sender.send('task:log', text));
});

ipcMain.handle('cloud:health', async (_, serverUrl) => {
  const res = await fetch(`${serverUrl}/health`);
  return res.json();
});

ipcMain.handle('cloud:list', async (_, serverUrl) => {
  const res = await fetch(`${serverUrl}/list`);
  if (!res.ok) throw new Error(`list failed: ${res.status}`);
  return res.json();
});

ipcMain.handle('cloud:upload', async (_, payload) => {
  const data = fs.readFileSync(payload.filePath);
  const filename = path.basename(payload.filePath);
  const url = `${payload.serverUrl}/upload?filename=${encodeURIComponent(filename)}&maxBackups=${encodeURIComponent(payload.maxBackups || 10)}`;
  const res = await fetch(url, { method: 'POST', body: data });
  if (!res.ok) throw new Error(`upload failed: ${res.status}`);
  return res.json();
});

ipcMain.handle('cloud:download', async (_, payload) => {
  ensureDirs();
  const res = await fetch(`${payload.serverUrl}/download/${encodeURIComponent(payload.filename)}`);
  if (!res.ok) throw new Error(`download failed: ${res.status}`);
  const buffer = Buffer.from(await res.arrayBuffer());
  const target = path.join(downloadRoot, payload.filename);
  fs.writeFileSync(target, buffer);
  return { success: true, filePath: target };
});

ipcMain.handle('cloud:delete', async (_, payload) => {
  const res = await fetch(`${payload.serverUrl}/delete/${encodeURIComponent(payload.filename)}`, { method: 'DELETE' });
  if (!res.ok) throw new Error(`delete failed: ${res.status}`);
  return res.json();
});

ipcMain.handle('cloud:setMaxBackups', async (_, payload) => {
  const res = await fetch(`${payload.serverUrl}/config/max-backups`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ maxBackups: payload.maxBackups })
  });
  if (!res.ok) throw new Error(`config failed: ${res.status}`);
  return res.json();
});
