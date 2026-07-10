const $ = (id) => document.getElementById(id);

let scheduleTimer = null;
let selectedRecord = null;

function appendLog(text) {
  const stamp = new Date().toLocaleString();
  $('log').textContent += `[${stamp}] ${text.trim()}\n`;
  $('log').scrollTop = $('log').scrollHeight;
}

function formatBytes(value) {
  const units = ['B', 'KB', 'MB', 'GB'];
  let n = Number(value || 0);
  let i = 0;
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024;
    i += 1;
  }
  return `${n.toFixed(i === 0 ? 0 : 2)} ${units[i]}`;
}

function formConfig() {
  return {
    serverUrl: $('serverUrl').value.trim(),
    maxBackups: Number($('maxBackups').value || 10),
    scheduleInterval: Number($('scheduleInterval').value || 60),
    lastSourceDir: $('sourceDir').value.trim(),
    lastRestoreDir: $('restoreDir').value.trim(),
    corePath: $('corePath').value.trim()
  };
}

function backupPayload() {
  return {
    sourceDir: $('sourceDir').value.trim(),
    packageName: $('packageName').value.trim(),
    password: $('backupPassword').value,
    filter: {
      extensions: $('extensions').value.trim(),
      nameContains: $('nameContains').value.trim(),
      minSize: Number($('minSize').value || 0),
      maxSize: Number($('maxSize').value || 0),
      modifiedAfter: $('modifiedAfter').value.trim()
    }
  };
}

async function saveConfig() {
  await window.backupApi.saveConfig(formConfig());
  appendLog('配置已保存');
}

async function refreshList() {
  const data = await window.backupApi.list($('serverUrl').value.trim());
  const rows = $('backupRows');
  rows.innerHTML = '';
  for (const record of data.records || []) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${record.filename}</td>
      <td>${formatBytes(record.size)}</td>
      <td>${record.uploadTime || ''}</td>
      <td>${record.lastAccessTime || ''}</td>
      <td class="actions"></td>
    `;
    const actions = tr.querySelector('.actions');
    const select = document.createElement('button');
    select.textContent = '选择';
    select.addEventListener('click', () => {
      selectedRecord = record;
      $('selectedFile').value = record.filename;
      appendLog(`已选择云端备份：${record.filename}`);
    });
    const remove = document.createElement('button');
    remove.textContent = '删除';
    remove.className = 'delete';
    remove.addEventListener('click', async () => {
      await window.backupApi.deleteFile({ serverUrl: $('serverUrl').value.trim(), filename: record.filename });
      appendLog(`已删除：${record.filename}`);
      await refreshList();
    });
    actions.append(select, remove);
    rows.appendChild(tr);
  }
}

async function runBackupOnce() {
  await saveConfig();
  if (!$('sourceDir').value.trim()) throw new Error('请选择源目录');
  if (!$('backupPassword').value) throw new Error('请输入备份密码');
  const listData = await window.backupApi.list($('serverUrl').value.trim());
  const existingNames = (listData.records || []).map((record) => record.filename);
  appendLog('开始调用 C++ 核心程序生成备份包');
  const payload = backupPayload();
  payload.existingNames = existingNames;
  const result = await window.backupApi.backup(payload);
  if (!result.success) throw new Error(result.message || '备份失败');
  appendLog(`本地备份完成：${result.outputFile}`);
  appendLog('开始上传云端');
  await window.backupApi.upload({
    serverUrl: $('serverUrl').value.trim(),
    filePath: result.outputFile,
    maxBackups: Number($('maxBackups').value || 10)
  });
  appendLog('上传完成');
  await refreshList();
}

async function runRestore() {
  await saveConfig();
  const filename = $('selectedFile').value.trim();
  if (!filename) throw new Error('请先选择云端备份包');
  if (!$('restoreDir').value.trim()) throw new Error('请选择还原目录');
  if (!$('restorePassword').value) throw new Error('请输入还原密码');
  appendLog(`开始下载：${filename}`);
  const downloaded = await window.backupApi.download({ serverUrl: $('serverUrl').value.trim(), filename });
  appendLog(`下载完成：${downloaded.filePath}`);
  const result = await window.backupApi.restore({
    inputFile: downloaded.filePath,
    restoreDir: $('restoreDir').value.trim(),
    password: $('restorePassword').value
  });
  if (!result.success) throw new Error(result.message || '还原失败');
  appendLog(`还原完成：${result.restoredCount} 个文件，校验失败 ${result.hashFailedCount} 个`);
}

async function guarded(task) {
  try {
    await task();
  } catch (err) {
    appendLog(`错误：${err.message}`);
  }
}

window.backupApi.onLog((text) => appendLog(text));

window.addEventListener('DOMContentLoaded', async () => {
  const config = await window.backupApi.loadConfig();
  $('serverUrl').value = config.serverUrl || 'http://127.0.0.1:8080';
  $('maxBackups').value = config.maxBackups || 10;
  $('scheduleInterval').value = config.scheduleInterval || 60;
  $('sourceDir').value = config.lastSourceDir || '';
  $('restoreDir').value = config.lastRestoreDir || '';
  $('corePath').value = config.corePath || '';

  $('saveConfig').addEventListener('click', () => guarded(saveConfig));
  $('checkServer').addEventListener('click', () => guarded(async () => {
    const result = await window.backupApi.health($('serverUrl').value.trim());
    $('serverState').textContent = result.status === 'ok' ? '服务器在线' : '服务器异常';
    appendLog(`服务器状态：${JSON.stringify(result)}`);
  }));
  $('chooseSource').addEventListener('click', () => guarded(async () => {
    const dir = await window.backupApi.chooseDirectory('选择源目录');
    if (dir) $('sourceDir').value = dir;
  }));
  $('chooseRestore').addEventListener('click', () => guarded(async () => {
    const dir = await window.backupApi.chooseDirectory('选择还原目录');
    if (dir) $('restoreDir').value = dir;
  }));
  $('runBackup').addEventListener('click', () => guarded(runBackupOnce));
  $('runRestore').addEventListener('click', () => guarded(runRestore));
  $('refreshList').addEventListener('click', () => guarded(refreshList));
  $('clearLog').addEventListener('click', () => { $('log').textContent = ''; });
  $('startSchedule').addEventListener('click', () => guarded(async () => {
    if (scheduleTimer) clearInterval(scheduleTimer);
    const minutes = Math.max(1, Number($('scheduleInterval').value || 60));
    scheduleTimer = setInterval(() => guarded(runBackupOnce), minutes * 60 * 1000);
    $('scheduleState').textContent = `已启动，每 ${minutes} 分钟执行`;
    appendLog('定时备份已启动');
  }));
  $('stopSchedule').addEventListener('click', () => {
    if (scheduleTimer) clearInterval(scheduleTimer);
    scheduleTimer = null;
    $('scheduleState').textContent = '未启动';
    appendLog('定时备份已停止');
  });
});
