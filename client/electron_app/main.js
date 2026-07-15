//main.js —— Electron 桌面客户端主进程入口

//职责：
//  1. 创建并管理应用窗口
//  2. 读写本地持久化配置（client_config.json）
//  3. 调用 C++ 核心程序（backup_core）完成本地备份/还原
//  4. 通过 HTTP 与远程备份服务器通信（上传/下载/删除）
//  5. 通过 IPC 将上述能力暴露给渲染进程

// 安全模型：
//  - 开启 contextIsolation，关闭 nodeIntegration
//  - 渲染进程只能通过 preload.js 暴露的 window.backupApi 与主进程交互


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

//获取内置 C++ 核心程序的绝对路径
//包后位于 resources/bin/，开发环境位于项目根目录的 build/
function bundledCorePath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'bin', coreName);
  }
  return path.resolve(__dirname, '..', '..', 'build', coreName);
}

//优先使用用户保存的路径，若无效则回退到内置路径
function resolveCorePath(savedCorePath) {
  const fallback = bundledCorePath();
  if (savedCorePath && fs.existsSync(savedCorePath)) {
    return savedCorePath;
  }
  return fallback;
}

//确保运行时目录（临时、下载、配置）存在
function ensureDirs() {
  fs.mkdirSync(tempRoot, { recursive: true });
  fs.mkdirSync(downloadRoot, { recursive: true });
  fs.mkdirSync(configDir, { recursive: true });
}

//读取客户端配置,文件不存在则返回默认值，存在则与默认值合并（用户值优先）
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

//将配置对象持久化到 client_config.json
function saveConfig(config) {
  ensureDirs();
  fs.writeFileSync(configFile, JSON.stringify(config, null, 2));
}

// 创建应用主窗口
//开启 contextIsolation、关闭 nodeIntegration 以保障安全 
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

//应用生命周期管理

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


//启动 C++ 核心程序并等待执行完毕
//使用 spawn 启动子进程，实时收集 stdout/stderr
//子进程结束后从输出末尾查找 JSON 结果行进行解析
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
      try { parsed = jsonLine ? JSON.parse(jsonLine) : {}; } catch (_) { }
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

//清理备份包名称
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


//确保唯一名，重名自动加序号
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


//将筛选规则写入临时 JSON 文件，供 C++ 核心读取
function writeFilterFile(filter) {
  const file = path.join(tempRoot, `filter_${Date.now()}.json`);
  fs.writeFileSync(file, JSON.stringify(filter, null, 2));
  return file;
}

//本地备份（生成唯一包名 → 写筛选规则 → 调 C++ 核心打包加密）
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

//本地还原（解密 + 解包，overwrite=replace)
ipcMain.handle('core:restore', async (event, payload) => {
  return runCore([
    'restore',
    '--input', payload.inputFile,
    '--restore', payload.restoreDir,
    '--password', payload.password,
    '--overwrite', 'replace'
  ], (text) => event.sender.send('task:log', text));
});

// 云端通信（HTTP 请求 backup_server）

// 健康检查：GET /health
ipcMain.handle('cloud:health', async (_, serverUrl) => {
  const res = await fetch(`${serverUrl}/health`);
  return res.json();
});

// 获取云端备份列表：GET /list
ipcMain.handle('cloud:list', async (_, serverUrl) => {
  const res = await fetch(`${serverUrl}/list`);
  if (!res.ok) throw new Error(`list failed: ${res.status}`);
  return res.json();
});

// 上传备份包到云端（流式上传）
ipcMain.handle('cloud:upload', async (_, payload) => {
  const filename = path.basename(payload.filePath);
  const stat = fs.statSync(payload.filePath);
  const url = `${payload.serverUrl}/upload?filename=${encodeURIComponent(filename)}&maxBackups=${encodeURIComponent(payload.maxBackups || 10)}`;
  const res = await fetch(url, {
    method: 'POST',
    body: fs.createReadStream(payload.filePath),//使用 createReadStream 避免大文件占满内存
    duplex: 'half',
    headers: {
      'Content-Type': 'application/octet-stream',
      'Content-Length': String(stat.size)//文件大小
    }
  });
  if (!res.ok) throw new Error(`upload failed: ${res.status}`);
  return res.json();
});

// 从云端下载备份包到本地下载目录
ipcMain.handle('cloud:download', async (_, payload) => {
  ensureDirs();
  const res = await fetch(`${payload.serverUrl}/download/${encodeURIComponent(payload.filename)}`);
  if (!res.ok) throw new Error(`download failed: ${res.status}`);
  const buffer = Buffer.from(await res.arrayBuffer());
  const target = path.join(downloadRoot, payload.filename);
  fs.writeFileSync(target, buffer);
  return { success: true, filePath: target };
});

// 删除云端备份：DELETE /delete/<filename>
ipcMain.handle('cloud:delete', async (_, payload) => {
  const res = await fetch(`${payload.serverUrl}/delete/${encodeURIComponent(payload.filename)}`, { method: 'DELETE' });
  if (!res.ok) throw new Error(`delete failed: ${res.status}`);
  return res.json();
});

// 设置服务器端最大备份保留数（LRU 淘汰上限）
ipcMain.handle('cloud:setMaxBackups', async (_, payload) => {
  const res = await fetch(`${payload.serverUrl}/config/max-backups`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ maxBackups: payload.maxBackups })
  });
  if (!res.ok) throw new Error(`config failed: ${res.status}`);
  return res.json();
});
