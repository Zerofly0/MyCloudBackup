# 个人云备份系统

本项目根据《软件开发综合实验》要求和项目需求/设计文档实现，包含三个部分：

- `backup_core`：C++17 本地核心程序，负责目录扫描、文件筛选、manifest 生成、打包、加密、解密、解包、还原和 Hash 校验。
- `backup_server`：C++17 云端备份服务器，提供 `/health`、`/upload`、`/list`、`/download/{filename}`、`/delete/{filename}`、`/config/max-backups` 接口，并维护 `metadata.json` 和 LRU 淘汰策略。
- `client/electron_app`：Electron 桌面客户端源码，负责图形界面、参数输入、日志显示、定时备份、核心程序调用和云端通信。

## 功能覆盖

- 基础要求：数据备份、数据还原。
- 自定义备份：按路径、扩展名、文件名关键词、文件大小、修改时间筛选。
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

## 新手配置与启动部署

本项目由两部分组成：

- 本地客户端：在 Windows 电脑上运行 Electron 图形界面，并调用 `backup_core.exe` 完成备份和还原。
- 云服务器端：在 Linux 云服务器上运行 `backup_server`，负责保存、下载、删除云端备份包。

如果只是使用已经部署好的云服务器，通常只需要完成“Windows 客户端配置”。如果需要重新部署服务器，再看“云服务器配置”。

### 一、Windows 客户端配置

#### 1. 准备环境

Windows 电脑需要安装：

```text
Node.js，包含 npm
MinGW-w64 g++，用于编译 C++ 核心程序
PowerShell
Git，可选，用于拉取代码
```

检查命令：

```powershell
node -v
npm -v
g++ --version
git --version
```

如果 `g++ --version` 找不到命令，需要把 MinGW-w64 的 `bin` 目录加入系统环境变量 `Path`。

#### 2. 获取项目代码

方式一：从 GitHub 克隆：

```powershell
cd F:\vscodework
git clone https://github.com/Zerofly0/MyCloudBackup.git
cd F:\vscodework\MyCloudBackup
```

方式二：直接复制整个项目文件夹到本机，例如：

```text
F:\vscodework\MyCloudBackup
```

后续命令都以该路径为例。如果你放在 `D:\MyCloudBackup`，把命令里的路径替换成自己的实际路径即可。

#### 3. 编译 C++ 核心程序

在项目根目录执行：

```powershell
cd F:\vscodework\MyCloudBackup
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_windows.ps1
```

成功后会生成：

```text
F:\vscodework\MyCloudBackup\build\backup_core.exe
```

这个文件是客户端实际调用的本地备份/还原核心程序。

#### 4. 安装 Electron 客户端依赖

注意：`npm install` 要在 `client\electron_app` 目录执行，不是在项目根目录执行。

```powershell
cd F:\vscodework\MyCloudBackup\client\electron_app
$env:ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
npm install
```

如果下载慢或失败，可以先设置 npm 镜像：

```powershell
npm config set registry https://registry.npmmirror.com
$env:ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
npm install
```

#### 5. 启动客户端

```powershell
cd F:\vscodework\MyCloudBackup\client\electron_app
npm start
```

客户端启动后，在左侧配置：

```text
服务器地址：http://8.137.100.109:8080
C++ 核心程序：F:\vscodework\MyCloudBackup\build\backup_core.exe
最大云端保留数：10
```

点击“检查服务器”，如果显示服务器在线，就可以进行备份、上传、下载和还原。

### 二、连接已有云服务器

当前项目已经使用的云服务器地址是：

```text
http://8.137.100.109:8080
```

在本机 PowerShell 中测试：

```powershell
curl.exe http://8.137.100.109:8080/health
```

正常返回类似：

```json
{"status":"ok"}
```

如果连接失败，可能原因有：

- 云服务器上的 `backup_server` 没有启动。
- 阿里云安全组没有开放 TCP 8080 端口。
- 服务器防火墙阻止了 8080 端口。
- 服务器程序崩溃，需要查看 `server.log`。

### 三、云服务器配置

如果服务器已经部署好，组员一般不需要执行本节。只有需要重新部署服务端时才执行。

#### 1. 登录服务器

```powershell
ssh root@8.137.100.109
```

#### 2. 安装基础环境

在 Ubuntu 服务器上执行：

```bash
apt update
apt install -y build-essential cmake git curl
```

#### 3. 上传或拉取项目代码

方式一：从 GitHub 拉取：

```bash
cd ~
git clone https://github.com/Zerofly0/MyCloudBackup.git
cd ~/MyCloudBackup
```

如果已经存在旧目录，可以进入目录后更新：

```bash
cd ~/MyCloudBackup
git pull
```

方式二：从 Windows 上传：

```powershell
scp -r F:\vscodework\MyCloudBackup root@8.137.100.109:~/MyCloudBackup
```

注意：`scp` 命令要在 Windows PowerShell 中执行，不要在已经登录的服务器终端里执行。

#### 4. 编译服务器程序

```bash
cd ~/MyCloudBackup
cmake -S . -B build
cmake --build build
```

成功后会生成：

```text
~/MyCloudBackup/build/backup_server
~/MyCloudBackup/build/backup_core
```

#### 5. 启动服务器

```bash
cd ~/MyCloudBackup
nohup ./build/backup_server 8080 ./backup_server_data > server.log 2>&1 &
```

检查是否启动成功：

```bash
curl http://127.0.0.1:8080/health
```

查看日志：

```bash
tail -f server.log
```

查看进程和端口：

```bash
ps -ef | grep backup_server
ss -lntp | grep 8080
```

如果端口已经被占用，可以先停止旧服务：

```bash
pkill backup_server
```

然后重新启动：

```bash
nohup ./build/backup_server 8080 ./backup_server_data > server.log 2>&1 &
```

#### 6. 开放云服务器端口

在阿里云控制台的安全组中添加入方向规则：

```text
协议类型：TCP
端口范围：8080/8080
授权对象：0.0.0.0/0
```

开放后，在本机 PowerShell 测试：

```powershell
curl.exe http://8.137.100.109:8080/health
```

### 四、本地单机运行方式

如果暂时不想使用云服务器，也可以把服务器运行在自己电脑上。

一个 PowerShell 窗口启动本地服务端：

```powershell
cd F:\vscodework\MyCloudBackup
.\build\backup_server.exe 8080 .\backup_server_data
```

另一个 PowerShell 窗口启动客户端：

```powershell
cd F:\vscodework\MyCloudBackup\client\electron_app
npm start
```

客户端服务器地址填写：

```text
http://127.0.0.1:8080
```

注意：Windows 下是否能生成 `backup_server.exe` 取决于当前 CMake 和编译环境。课程演示中更推荐服务器端在 Ubuntu 云服务器运行，Windows 本机主要运行 Electron 客户端和 `backup_core.exe`。

### 五、命令行备份和还原

备份命令：

```powershell
cd F:\vscodework\MyCloudBackup
.\build\backup_core.exe backup `
  --source "F:\vscodework\MyCloudBackup\test_data" `
  --output "F:\vscodework\MyCloudBackup\backup_test.bak.enc" `
  --password "123456"
```

还原命令：

```powershell
cd F:\vscodework\MyCloudBackup
.\build\backup_core.exe restore `
  --input "F:\vscodework\MyCloudBackup\backup_test.bak.enc" `
  --restore "F:\vscodework\MyCloudBackup\restore_test" `
  --password "123456"
```

备份和还原必须使用同一个密码。

### 六、常见问题

#### npm install 找不到 package.json

原因：在项目根目录执行了 `npm install`。

正确做法：

```powershell
cd F:\vscodework\MyCloudBackup\client\electron_app
npm install
```

#### Electron 下载失败或 ECONNRESET

使用 Electron 镜像后重新安装：

```powershell
$env:ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
npm install
```

#### 服务器连接不上

先在本机测试：

```powershell
curl.exe http://8.137.100.109:8080/health
```

再登录服务器检查：

```bash
ssh root@8.137.100.109
cd ~/MyCloudBackup
ps -ef | grep backup_server
ss -lntp | grep 8080
tail -n 50 server.log
```

如果服务没有运行，重新启动：

```bash
nohup ./build/backup_server 8080 ./backup_server_data > server.log 2>&1 &
```

#### 备份成功但还原失败

优先检查：

```text
备份密码和还原密码是否完全一致
C++ 核心程序路径是否指向 build\backup_core.exe
还原目录是否可写
备份包是否下载完整
是否使用了重新编译后的 backup_core.exe
```

#### 修改 C++ 代码后没有生效

修改 `src/core/backup_core.cpp` 后，需要重新编译：

```powershell
cd F:\vscodework\MyCloudBackup
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build_windows.ps1
```

然后重启 Electron 客户端。
