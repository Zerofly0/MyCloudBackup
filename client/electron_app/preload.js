// preload.js —— Electron 预加载脚本（安全桥接层）

// 在渲染进程加载之前执行，拥有 Node.js 权限但与渲染进程隔离
// 通过 contextBridge 将主进程能力以白名单 API 暴露到 window.backupApi
// 渲染进程无法直接使用 require / Node API，保障安全
const { contextBridge, ipcRenderer } = require('electron');

//暴露 API 到渲染进程的 window.backupApi,每个方法内部调用 ipcRenderer.invoke 向主进程发送 IPC 请求
contextBridge.exposeInMainWorld('backupApi', {
  // 配置管理
  loadConfig: () => ipcRenderer.invoke('config:load'),
  saveConfig: (config) => ipcRenderer.invoke('config:save', config),
  // 系统对话框
  chooseDirectory: (title) => ipcRenderer.invoke('dialog:directory', title),
  // 本地备份/还原（调用 C++ 核心程序）
  backup: (payload) => ipcRenderer.invoke('core:backup', payload),
  restore: (payload) => ipcRenderer.invoke('core:restore', payload),
  // 云端操作（HTTP 请求 backup_server）
  health: (serverUrl) => ipcRenderer.invoke('cloud:health', serverUrl),
  list: (serverUrl) => ipcRenderer.invoke('cloud:list', serverUrl),
  upload: (payload) => ipcRenderer.invoke('cloud:upload', payload),
  download: (payload) => ipcRenderer.invoke('cloud:download', payload),
  deleteFile: (payload) => ipcRenderer.invoke('cloud:delete', payload),
  setMaxBackups: (payload) => ipcRenderer.invoke('cloud:setMaxBackups', payload),
  // 日志监听：接收主进程推送的 C++ 核心实时输出
  onLog: (handler) => ipcRenderer.on('task:log', (_, text) => handler(text))
});
