//renderer.js —— 渲染进程主逻辑（UI 交互层）

//职责：
//   1. 页面加载时初始化配置（从主进程读取并填充表单）
//   2. 绑定所有 UI 按钮的事件监听器
//   3. 通过 window.backupApi 调用主进程能力
//   4. 管理云端列表展示、选择、删除
//   5. 编排「备份→上传」和「下载→还原」完整流程
//   6. 管理定时备份、日志显示与错误捕获

//快捷 DOM 查询，等价于 document.getElementById
const $ = (id) => document.getElementById(id);

//定时备份的 setInterval 句柄 
let scheduleTimer = null;

//当前在云端列表中「已选择」的备份记录，用于还原操作
let selectedRecord = null;

//向日志区追加一行带时间戳的文本，自动滚动到底部
function appendLog(text) {
  const stamp = new Date().toLocaleString();
  $('log').textContent += `[${stamp}] ${text.trim()}\n`;
  $('log').scrollTop = $('log').scrollHeight;
}

//将字节数转化为传统单位
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

//从表单收集当前配置，构建配置对象
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

//从表单收集备份参数，包含源目录、包名、密码、文件筛选规则
function backupPayload() {
  return {
    sourceDir: $('sourceDir').value.trim(),
    password: $('backupPassword').value,
    packageName: $('packageName').value.trim(),
    filter: {
      extensions: $('extensions').value.trim(),
      nameContains: $('nameContains').value.trim(),
      minSize: Number($('minSize').value || 0),
      maxSize: Number($('maxSize').value || 0),
      modifiedAfter: $('modifiedAfter').value.trim()
    }
  };
}

//保存当前表单配置到本地持久化文件
async function saveConfig() {
  await window.backupApi.saveConfig(formConfig());
  appendLog('配置已保存');
}

//刷新云端备份列表
//从服务器获取最新记录，重新渲染表格行
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
    //每行包含「选择」和「删除」两个操作按钮
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

//带重试的列表刷新
//备份上传后服务器可能尚未完成索引，最多重试 3 次，每次间隔 1 秒
async function refreshListWithRetry(retries = 3, delayMs = 1000) {
  for (let attempt = 1; attempt <= retries; attempt += 1) {
    try {
      await refreshList();
      return true;
    } catch (err) {
      if (attempt < retries) {
        await new Promise((resolve) => setTimeout(resolve, delayMs));
      }
    }
  }
  return false;
}

//执行单次「备份并上传」完整流程
//保存配置 → 校验输入 → 获取云端已有文件名（去重）→ 调 C++ 核心打包加密 → 上传云端 → 刷新列表
async function runBackupOnce() {
  await saveConfig();
  if (!$('sourceDir').value.trim()) throw new Error('请选择源目录');
  if (!$('backupPassword').value) throw new Error('请输入备份密码');
  let existingNames = [];
  try {
    const data = await window.backupApi.list($('serverUrl').value.trim());
    existingNames = (data.records || []).map((record) => record.filename);
  } catch (err) {
    existingNames = [];
  }
  appendLog('开始调用 C++ 核心程序生成备份包');
  const result = await window.backupApi.backup({ ...backupPayload(), existingNames });
  if (!result.success) throw new Error(result.message || '备份失败');
  appendLog(`本地备份完成：${result.outputFile}`);
  appendLog('开始上传云端');
  await window.backupApi.upload({
    serverUrl: $('serverUrl').value.trim(),
    filePath: result.outputFile,
    maxBackups: Number($('maxBackups').value || 10)
  });
  appendLog('上传完成');
  await refreshListWithRetry();
}

//执行「下载并还原」完整流程
//保存配置 → 校验输入 → 从云端下载 → 调 C++ 核心解密还原
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

//统一错误捕获包装器，避免未捕获的 Promise 异常导致程序崩溃
async function guarded(task) {
  try {
    await task();
  } catch (err) {
    appendLog(`错误：${err.message}`);
  }
}

//注册主进程日志推送监听
window.backupApi.onLog((text) => appendLog(text));

//页面加载完成后初始化 UI
window.addEventListener('DOMContentLoaded', async () => {
  // 从主进程加载持久化配置，填充表单
  const config = await window.backupApi.loadConfig();
  $('serverUrl').value = config.serverUrl || 'http://127.0.0.1:8080';
  $('maxBackups').value = config.maxBackups || 10;
  $('scheduleInterval').value = config.scheduleInterval || 60;
  $('sourceDir').value = config.lastSourceDir || '';
  $('restoreDir').value = config.lastRestoreDir || '';
  $('corePath').value = config.corePath || '';

  //绑定按钮事件
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
