const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('backupApi', {
  loadConfig: () => ipcRenderer.invoke('config:load'),
  saveConfig: (config) => ipcRenderer.invoke('config:save', config),
  chooseDirectory: (title) => ipcRenderer.invoke('dialog:directory', title),
  backup: (payload) => ipcRenderer.invoke('core:backup', payload),
  restore: (payload) => ipcRenderer.invoke('core:restore', payload),
  health: (serverUrl) => ipcRenderer.invoke('cloud:health', serverUrl),
  list: (serverUrl) => ipcRenderer.invoke('cloud:list', serverUrl),
  upload: (payload) => ipcRenderer.invoke('cloud:upload', payload),
  download: (payload) => ipcRenderer.invoke('cloud:download', payload),
  deleteFile: (payload) => ipcRenderer.invoke('cloud:delete', payload),
  setMaxBackups: (payload) => ipcRenderer.invoke('cloud:setMaxBackups', payload),
  onLog: (handler) => ipcRenderer.on('task:log', (_, text) => handler(text))
});
