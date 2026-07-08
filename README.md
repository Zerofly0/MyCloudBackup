# 个人云备份系统

本项目根据《软件开发综合实验》要求和项目需求/设计文档实现，包含三个部分：

- `backup_core`：C++17 本地核心程序，负责目录扫描、文件筛选、manifest 生成、打包、加密、解密、解包、还原和 Hash 校验。
- `backup_server`：C++17 云端备份服务器，提供 `/health`、`/upload`、`/list`、`/download/{filename}`、`/delete/{filename}`、`/config/max-backups` 接口，并维护 `metadata.json` 和 LRU 淘汰策略。
- `client/electron_app`：Electron 桌面客户端源码，负责图形界面、参数输入、日志显示、定时备份、核心程序调用和云端通信。

## 功能覆盖

- 基础要求：数据备份、数据还原。
- 自定义备份：按扩展名、文件名关键词、文件大小、修改时间筛选。
- 打包解包：自定义 `.bak` 包格式，包含 magic、版本、manifest 长度、manifest JSON 和文件数据块。
- 加密解密：`.bak.enc` 包格式，包含 salt、nonce、认证标签和密文；密钥由用户密码经 PBKDF2-HMAC-SHA256 派生。
- 元数据管理：manifest 记录相对路径、大小、SHA-256、修改时间、偏移和长度；服务器 metadata 记录文件名、大小、上传时间、最近使用时间、包 Hash 和存储路径。
- 图形界面：Electron 客户端支持备份上传、列表刷新、下载还原、删除、定时备份和日志显示。
- 数据淘汰：服务器按 `lastAccessTime` 执行 LRU 淘汰。

## 构建

### Windows 客户端核心程序

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_windows.ps1
```

生成文件通常位于：

```text
build/backup_core.exe
```

如果使用 Visual Studio Developer PowerShell，也可以通过 CMake 构建。MinGW 的 `mingw32-make` 在中文项目路径下可能出现编码问题，因此 Windows 演示优先使用 `scripts/build_windows.ps1`。

### Linux 服务器和核心程序

```bash
cmake -S . -B build
cmake --build build
```

生成：

```text
build/backup_core
build/backup_server
```

## 命令行使用

### 备份

```bash
./backup_core backup \
  --source ./sample_data \
  --output ./temp/backup_20260707_150000.bak.enc \
  --password 123456 \
  --filter ./filter.json
```

筛选配置示例：

```json
{
  "extensions": ".txt,.cpp,.docx",
  "nameContains": "",
  "minSize": 0,
  "maxSize": 0,
  "modifiedAfter": ""
}
```

### 还原

```bash
./backup_core restore \
  --input ./temp/backup_20260707_150000.bak.enc \
  --restore ./restore_out \
  --password 123456
```

## 服务器运行

```bash
./backup_server 8080 ./backup_server_data
```

接口示例：

```bash
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/list
curl -X POST --data-binary @backup.bak.enc "http://127.0.0.1:8080/upload?filename=backup.bak.enc&maxBackups=10"
curl -O http://127.0.0.1:8080/download/backup.bak.enc
curl -X DELETE http://127.0.0.1:8080/delete/backup.bak.enc
```

## Electron 客户端运行

进入客户端目录：

```bash
cd client/electron_app
npm install
npm start
```

第一次启动后，在左侧配置：

- 服务器地址：例如 `http://127.0.0.1:8080`
- C++ 核心程序：填写已构建的 `backup_core` 或 `backup_core.exe` 路径
- 最大云端保留数：默认 `10`

客户端不会保存用户密码，只保存服务器地址、核心程序路径、目录和定时备份间隔等非敏感配置。

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── client/electron_app
│   ├── index.html
│   ├── main.js
│   ├── package.json
│   ├── preload.js
│   ├── renderer.js
│   └── styles.css
├── scripts
│   └── smoke_test.ps1
└── src
    ├── core/backup_core.cpp
    └── server/backup_server.cpp
```

## 说明

项目不使用数据库，服务器端元数据保存在 `metadata.json`。当前实现以课程演示和个人轻量级备份为目标，不包含多用户账号、文件分享、在线预览、增量备份、实时同步、压缩和 HTTPS。
