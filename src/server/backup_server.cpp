#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

struct Record {
    std::string filename;
    uint64_t size = 0;
    std::string uploadTime;
    std::string lastAccessTime;
    std::string packageHash;
    std::string storagePath;
};

struct ServerState {
    fs::path root = "backup_server";
    fs::path backups = root / "backups";
    fs::path metadata = root / "metadata.json";
    int maxBackups = 10;
    std::vector<Record> records;
};

static ServerState state;

static std::string nowText() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

static std::string escapeJson(const std::string& s) {
    std::ostringstream out;
    for (char c : s) {
        if (c == '\\') out << "\\\\";
        else if (c == '"') out << "\\\"";
        else if (c == '\n') out << "\\n";
        else out << c;
    }
    return out.str();
}

class Sha256 {
public:
    Sha256() { h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; }
    void update(const uint8_t* data, size_t len) {
        bits += len * 8;
        while (len) {
            size_t n = std::min(len, 64 - used);
            std::copy(data, data + n, buf.begin() + used);
            used += n; data += n; len -= n;
            if (used == 64) { transform(buf.data()); used = 0; }
        }
    }
    std::array<uint8_t, 32> final() {
        buf[used++] = 0x80;
        if (used > 56) { while (used < 64) buf[used++] = 0; transform(buf.data()); used = 0; }
        while (used < 56) buf[used++] = 0;
        for (int i = 7; i >= 0; --i) buf[used++] = (bits >> (i * 8)) & 0xff;
        transform(buf.data());
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[i*4] = (h[i] >> 24) & 0xff; out[i*4+1] = (h[i] >> 16) & 0xff;
            out[i*4+2] = (h[i] >> 8) & 0xff; out[i*4+3] = h[i] & 0xff;
        }
        return out;
    }
private:
    std::array<uint32_t, 8> h{};
    std::array<uint8_t, 64> buf{};
    size_t used = 0;
    uint64_t bits = 0;
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    void transform(const uint8_t* c) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (c[i*4]<<24)|(c[i*4+1]<<16)|(c[i*4+2]<<8)|c[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],cc=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i=0;i<64;++i){uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25);uint32_t ch=(e&f)^(~e&g);uint32_t t1=hh+s1+ch+k[i]+w[i];uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22);uint32_t maj=(a&b)^(a&cc)^(b&cc);uint32_t t2=s0+maj;hh=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=cc;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
};

static std::string sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Sha256 sha;
    std::array<uint8_t, 65536> b{};
    while (in) { in.read(reinterpret_cast<char*>(b.data()), b.size()); if (in.gcount() > 0) sha.update(b.data(), in.gcount()); }
    auto d = sha.final();
    std::ostringstream ss;
    for (auto x : d) ss << std::hex << std::setw(2) << std::setfill('0') << (int)x;
    return ss.str();
}

static bool safeName(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos && name.find("..") == std::string::npos;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss; ss << in.rdbuf(); return ss.str();
}

static std::string getJsonString(const std::string& obj, const std::string& key) {
    auto p = obj.find("\"" + key + "\""); if (p == std::string::npos) return "";
    auto c = obj.find(':', p); auto q1 = obj.find('"', c + 1); auto q2 = obj.find('"', q1 + 1);
    return q1 == std::string::npos || q2 == std::string::npos ? "" : obj.substr(q1 + 1, q2 - q1 - 1);
}

static uint64_t getJsonNumber(const std::string& obj, const std::string& key) {
    auto p = obj.find("\"" + key + "\""); if (p == std::string::npos) return 0;
    auto c = obj.find(':', p); auto s = obj.find_first_of("0123456789", c + 1);
    auto e = obj.find_first_not_of("0123456789", s); return s == std::string::npos ? 0 : std::stoull(obj.substr(s, e - s));
}

static std::string metadataJson() {
    std::ostringstream out;
    out << "{\n  \"maxBackups\":" << state.maxBackups << ",\n  \"records\":[\n";
    for (size_t i = 0; i < state.records.size(); ++i) {
        const auto& r = state.records[i];
        out << "    {\"filename\":\"" << escapeJson(r.filename) << "\",\"size\":" << r.size
            << ",\"uploadTime\":\"" << r.uploadTime << "\",\"lastAccessTime\":\"" << r.lastAccessTime
            << "\",\"packageHash\":\"" << r.packageHash << "\",\"storagePath\":\"" << escapeJson(r.storagePath) << "\"}";
        if (i + 1 < state.records.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

static void saveMetadata() {
    fs::create_directories(state.root);
    std::ofstream out(state.metadata);
    out << metadataJson();
}

static void loadMetadata() {
    fs::create_directories(state.backups);
    if (!fs::exists(state.metadata)) { saveMetadata(); return; }
    std::string text = readFile(state.metadata);
    state.maxBackups = static_cast<int>(getJsonNumber(text, "maxBackups"));
    if (state.maxBackups <= 0) state.maxBackups = 10;
    state.records.clear();
    size_t pos = text.find("\"records\"");
    while ((pos = text.find("{\"filename\"", pos)) != std::string::npos) {
        size_t end = text.find('}', pos);
        std::string obj = text.substr(pos, end - pos + 1);
        Record r;
        r.filename = getJsonString(obj, "filename");
        r.size = getJsonNumber(obj, "size");
        r.uploadTime = getJsonString(obj, "uploadTime");
        r.lastAccessTime = getJsonString(obj, "lastAccessTime");
        r.packageHash = getJsonString(obj, "packageHash");
        r.storagePath = getJsonString(obj, "storagePath");
        state.records.push_back(r);
        pos = end + 1;
    }
}

static void evictIfNeeded() {
    while (static_cast<int>(state.records.size()) > state.maxBackups) {
        auto it = std::min_element(state.records.begin(), state.records.end(), [](const Record& a, const Record& b) {
            return a.lastAccessTime < b.lastAccessTime;
        });
        if (it == state.records.end()) break;
        fs::remove(state.root / it->storagePath);
        state.records.erase(it);
    }
    saveMetadata();
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') out.push_back(' ');
        else out.push_back(s[i]);
    }
    return out;
}

static std::string queryParam(const std::string& target, const std::string& key) {
    auto q = target.find('?'); if (q == std::string::npos) return "";
    std::string query = target.substr(q + 1);
    std::stringstream ss(query);
    for (std::string item; std::getline(ss, item, '&');) {
        auto eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == key) return urlDecode(item.substr(eq + 1));
    }
    return "";
}

static void sendAll(int fd, const std::string& data) {
    const char* p = data.data();
    size_t n = data.size();
    while (n > 0) {
        ssize_t s = send(fd, p, n, 0);
        if (s <= 0) return;
        p += s; n -= static_cast<size_t>(s);
    }
}

static std::string response(const std::string& status, const std::string& type, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\nContent-Type: " << type << "\r\nContent-Length: " << body.size()
        << "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n\r\n" << body;
    return out.str();
}

static void sendFile(int fd, const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    uint64_t size = fs::file_size(path);
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " << size
           << "\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    sendAll(fd, header.str());
    std::array<char, 65536> buf{};
    while (in) { in.read(buf.data(), buf.size()); if (in.gcount() > 0) send(fd, buf.data(), in.gcount(), 0); }
}

static void handleClient(int fd) {
    std::string req;
    std::array<char, 8192> buf{};
    while (req.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) return;
        req.append(buf.data(), n);
        if (req.size() > 1024 * 1024) throw std::runtime_error("request header too large");
    }
    auto headEnd = req.find("\r\n\r\n");
    std::string head = req.substr(0, headEnd);
    std::string body = req.substr(headEnd + 4);
    std::istringstream hs(head);
    std::string method, target, version;
    hs >> method >> target >> version;
    size_t contentLength = 0;
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.rfind("content-length:", 0) == 0) contentLength = std::stoull(line.substr(15));
    }
    if (method == "OPTIONS") {
        sendAll(fd, response("204 No Content", "text/plain", ""));
    } else if (method == "GET" && target == "/health") {
        sendAll(fd, response("200 OK", "application/json", "{\"status\":\"ok\"}"));
    } else if (method == "GET" && target == "/list") {
        sendAll(fd, response("200 OK", "application/json", metadataJson()));
    } else if (method == "POST" && target.rfind("/upload", 0) == 0) {
        std::string filename = queryParam(target, "filename");
        if (!safeName(filename)) filename = "backup_" + std::to_string(std::time(nullptr)) + ".bak.enc";
        std::string maxText = queryParam(target, "maxBackups");
        if (!maxText.empty()) state.maxBackups = std::max(1, std::stoi(maxText));
        fs::path dest = state.backups / filename;
        std::ofstream out(dest, std::ios::binary);
        size_t written = std::min(body.size(), contentLength);
        if (written > 0) out.write(body.data(), static_cast<std::streamsize>(written));
        while (written < contentLength) {
            ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n <= 0) break;
            size_t remaining = contentLength - written;
            size_t toWrite = std::min(static_cast<size_t>(n), remaining);
            out.write(buf.data(), static_cast<std::streamsize>(toWrite));
            written += toWrite;
        }
        out.close();
        if (written != contentLength) {
            fs::remove(dest);
            sendAll(fd, response("400 Bad Request", "application/json", "{\"success\":false,\"message\":\"incomplete upload\"}"));
            return;
        }
        state.records.erase(std::remove_if(state.records.begin(), state.records.end(), [&](const Record& r) { return r.filename == filename; }), state.records.end());
        Record r;
        r.filename = filename;
        r.size = fs::file_size(dest);
        r.uploadTime = nowText();
        r.lastAccessTime = r.uploadTime;
        r.packageHash = sha256File(dest);
        r.storagePath = "backups/" + filename;
        state.records.push_back(r);
        evictIfNeeded();
        sendAll(fd, response("200 OK", "application/json", "{\"success\":true,\"message\":\"upload success\"}"));
    } else if (method == "GET" && target.rfind("/download/", 0) == 0) {
        std::string filename = urlDecode(target.substr(std::string("/download/").size()));
        if (!safeName(filename) || !fs::exists(state.backups / filename)) {
            sendAll(fd, response("404 Not Found", "application/json", "{\"success\":false,\"message\":\"file not found\"}"));
        } else {
            for (auto& r : state.records) if (r.filename == filename) r.lastAccessTime = nowText();
            saveMetadata();
            sendFile(fd, state.backups / filename);
        }
    } else if (method == "DELETE" && target.rfind("/delete/", 0) == 0) {
        std::string filename = urlDecode(target.substr(std::string("/delete/").size()));
        if (safeName(filename)) fs::remove(state.backups / filename);
        state.records.erase(std::remove_if(state.records.begin(), state.records.end(), [&](const Record& r) { return r.filename == filename; }), state.records.end());
        saveMetadata();
        sendAll(fd, response("200 OK", "application/json", "{\"success\":true,\"message\":\"delete success\"}"));
    } else if (method == "POST" && target == "/config/max-backups") {
        while (body.size() < contentLength) {
            ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n <= 0) break;
            body.append(buf.data(), n);
        }
        int value = static_cast<int>(getJsonNumber(body, "maxBackups"));
        if (value > 0) state.maxBackups = value;
        evictIfNeeded();
        sendAll(fd, response("200 OK", "application/json", "{\"success\":true,\"message\":\"config updated\"}"));
    } else {
        sendAll(fd, response("404 Not Found", "application/json", "{\"success\":false,\"message\":\"not found\"}"));
    }
}

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) port = std::stoi(argv[1]);
    if (argc > 2) state.root = argv[2];
    state.backups = state.root / "backups";
    state.metadata = state.root / "metadata.json";
    loadMetadata();
    std::signal(SIGPIPE, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) throw std::runtime_error("bind failed");
    if (listen(server, 16) != 0) throw std::runtime_error("listen failed");
    std::cout << "backup_server listening on http://0.0.0.0:" << port << std::endl;
    while (true) {
        int fd = accept(server, nullptr, nullptr);
        if (fd < 0) continue;
        try { handleClient(fd); }
        catch (const std::exception& e) { sendAll(fd, response("500 Internal Server Error", "application/json", std::string("{\"success\":false,\"message\":\"") + escapeJson(e.what()) + "\"}")); }
        close(fd);
    }
}
