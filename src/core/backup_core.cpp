#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

//解决路径字符串编码转换（中文路径），UTF-8->程序内部路径
static fs::path pathFromUtf8(const std::string& text) {
#ifdef _WIN32
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 0) return fs::path(text);
    std::wstring wide(static_cast<size_t>(needed - 1), L'\0');

    //转换
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), needed);

    return fs::path(wide);
#else
    return fs::path(text);
#endif
}

//解决路径字符串编码转换（中文路径），程序内部路径->UTF-8
static std::string pathToUtf8(const fs::path& path) {
#ifdef _WIN32
    std::wstring wide = path.wstring();
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string text(static_cast<size_t>(needed - 1), '\0');

    //转换
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(), needed, nullptr, nullptr);
    
    return text;
#else
    return path.string();
#endif
}

//宽字符文本->UTF-8
static std::string wideToUtf8(const std::wstring& wide) {
#ifdef _WIN32
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string text(static_cast<size_t>(needed - 1), '\0');

    //转换
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(), needed, nullptr, nullptr);

    return text;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

//manifest中每个文件的条目
struct FileEntry {
    std::string relativePath;//相对路径
    uint64_t size = 0;
    std::string sha256;//文件哈希值，用于完整性校验
    std::string modifiedTime;
    uint64_t offset = 0;
    uint64_t length = 0;
};

//筛选规则
struct FilterRule {
    std::vector<std::string> extensions;
    std::string nameContains;
    uint64_t minSize = 0;
    uint64_t maxSize = 0;
    std::string modifiedAfter;
};

//日志输出
static void logLine(const std::string& text) {
    std::cout << "[" << std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) << "] " << text << std::endl;
}

//读取文本文件
static std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

//写入4字节整数
static void writeU32(std::ostream& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xff));
}

//写入8字节整数
static void writeU64(std::ostream& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.put(static_cast<char>((v >> (i * 8)) & 0xff));
}

//读取4字节整数
static uint32_t readU32(std::istream& in) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(in.get())) << (i * 8);
    return v;
}

//读取8字节整数
static uint64_t readU64(std::istream& in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(in.get())) << (i * 8);
    return v;
}

//JSON字符串转义
static std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        if (c == '\\') out << "\\\\";
        else if (c == '"') out << "\\\"";
        else if (c == '\n') out << "\\n";
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;//控制字符
        else out << c;
    }
    return out.str();
}

//JSON字符串反转义
static std::string jsonUnescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'n') out.push_back('\n');
            else out.push_back(n);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

//获取当前系统时间
static std::string nowText() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

//获取文件最后修改时间
static std::string fileTimeText(const fs::path& path) {
    auto ftime = fs::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());//将文件系统时钟转换成系统时钟时间
    std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

//SHA256哈希算法，输入任意长度的数据，输出固定 32 字节
class Sha256 {
public:
    Sha256() { reset(); }

    // 输入数据（可分多次调用），内部按 64 字节分块进行压缩
    void update(const uint8_t* data, size_t len) {
        bitLen += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            size_t n = std::min(len, 64 - bufferLen);
            std::copy(data, data + n, buffer.begin() + bufferLen);
            bufferLen += n;
            data += n;
            len -= n;
            if (bufferLen == 64) {
                transform(buffer.data());
                bufferLen = 0;
            }
        }
    }
    void update(const std::vector<uint8_t>& data) { update(data.data(), data.size()); }
    
    // 完成哈希计算，返回 32 字节摘要
    std::array<uint8_t, 32> final() {
        buffer[bufferLen++] = 0x80;
        if (bufferLen > 56) {
            while (bufferLen < 64) buffer[bufferLen++] = 0;
            transform(buffer.data());
            bufferLen = 0;
        }
        while (bufferLen < 56) buffer[bufferLen++] = 0;
        for (int i = 7; i >= 0; --i) buffer[bufferLen++] = static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff);
        transform(buffer.data());
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
        }
        return out;
    }

    // 将摘要转换为十六进制字符串
    static std::string hex(const std::array<uint8_t, 32>& digest) {
        std::ostringstream ss;
        for (uint8_t b : digest) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
        return ss.str();
    }
private:
    std::array<uint32_t, 8> h{};//哈希状态
    std::array<uint8_t, 64> buffer{};//消息缓冲区
    size_t bufferLen = 0;
    uint64_t bitLen = 0;//已处理数据总位数
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    
    void reset() {
        h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        bufferLen = 0;
        bitLen = 0;
    }

    // 压缩函数：处理一个 64 字节的消息块，执行 64 轮迭代更新哈希状态
    void transform(const uint8_t* chunk) {
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
        for (int i = 0; i < 16; ++i) {
            w[i] = (chunk[i * 4] << 24) | (chunk[i * 4 + 1] << 16) | (chunk[i * 4 + 2] << 8) | chunk[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;
            hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};

//计算文件的SHA256哈希值
static std::string sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file for hash: " + pathToUtf8(path));
    
    Sha256 sha;
    std::array<uint8_t, 64 * 1024> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), buf.size());
        if (in.gcount() > 0) sha.update(buf.data(), static_cast<size_t>(in.gcount()));
    }
    return Sha256::hex(sha.final());
}

// HMAC-SHA256算法，输入密钥和数据，输出32字节的哈希值
static std::array<uint8_t, 32> hmacSha256(const std::vector<uint8_t>& key, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> k = key;

    // 如果密钥长度大于64字节，则先对其进行SHA256哈希
    if (k.size() > 64) {
        Sha256 sha;
        sha.update(k);
        auto d = sha.final();
        k.assign(d.begin(), d.end());
    }

    // 用0将密钥长度填充到64字节
    k.resize(64, 0);

    std::vector<uint8_t> ipad(64), opad(64);
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    //计算内层哈希
    Sha256 inner;
    inner.update(ipad);
    inner.update(data);
    auto id = inner.final();
    std::vector<uint8_t> innerData(id.begin(), id.end());

    //计算外层哈希
    Sha256 outer;
    outer.update(opad);
    outer.update(innerData);
    return outer.final();
}

// PBKDF2算法，输入密码、盐值、迭代次数和输出字节数，返回派生密钥
static std::vector<uint8_t> pbkdf2(const std::string& password, const std::vector<uint8_t>& salt, uint32_t iterations, size_t bytes) {
    std::vector<uint8_t> key(password.begin(), password.end());
    std::vector<uint8_t> out;
    
    // 生成足够多的密钥字节
    for (uint32_t block = 1; out.size() < bytes; ++block) {
        // 生成消息salt + block编号
        std::vector<uint8_t> msg = salt;
        msg.push_back((block >> 24) & 0xff);
        msg.push_back((block >> 16) & 0xff);
        msg.push_back((block >> 8) & 0xff);
        msg.push_back(block & 0xff);

        auto u = hmacSha256(key, msg);
        std::array<uint8_t, 32> t = u;
        for (uint32_t i = 1; i < iterations; ++i) {
            std::vector<uint8_t> prev(u.begin(), u.end());
            u = hmacSha256(key, prev);
            for (size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(bytes);
    return out;
}

static std::vector<uint8_t> randomBytes(size_t n) {
    std::random_device rd;
    std::vector<uint8_t> data(n);

    // 给每个字节填随机值
    for (auto& b : data) b = static_cast<uint8_t>(rd());
    return data;
}

//加密解密，HMAC-SHA256 生成密钥流后与数据异或的流式加密
static void cryptStream(std::vector<uint8_t>& data, const std::vector<uint8_t>& key, const std::vector<uint8_t>& nonce) {
    uint64_t counter = 0;
    size_t pos = 0;
    while (pos < data.size()) {
        std::vector<uint8_t> msg = nonce;
        for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((counter >> (i * 8)) & 0xff));
        auto block = hmacSha256(key, msg);
        for (size_t i = 0; i < block.size() && pos < data.size(); ++i) data[pos++] ^= block[i];
        ++counter;
    }
}

static bool constantEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    uint8_t r = 0;

    //避免时间差，进而避免时间攻击
    for (size_t i = 0; i < a.size(); ++i) r |= a[i] ^ b[i];
    return r == 0;
}

//把临时.bak文件完整读入内存，作为待加密明文
static std::vector<uint8_t> readBinary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read: " + pathToUtf8(path));
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//把一个字节数组写入指定文件
static void writeBinary(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write: " + pathToUtf8(path));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

//加密
static void encryptFile(const fs::path& input, const fs::path& output, const std::string& password) {
    std::vector<uint8_t> plain = readBinary(input);
    std::vector<uint8_t> salt = randomBytes(16);//随机盐，防止相同密码每次生成相同密钥
    std::vector<uint8_t> nonce = randomBytes(12);//一次性随机数，让加密过程每次不同，避免相同明文和相同密码产生完全相同的密文
    std::vector<uint8_t> key = pbkdf2(password, salt, 10000, 32);//用密码和salt生成密钥

    std::vector<uint8_t> cipher = plain;
    cryptStream(cipher, key, nonce);//用密钥和nonce对数据进行加密

    std::vector<uint8_t> tagData = salt;
    tagData.insert(tagData.end(), nonce.begin(), nonce.end());
    tagData.insert(tagData.end(), cipher.begin(), cipher.end());
    auto tagArr = hmacSha256(key, tagData);
    std::vector<uint8_t> tag(tagArr.begin(), tagArr.end());//用密钥对salt+nonce+cipher进行hmac生成tag
    
    std::ofstream out(output, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write encrypted output");
    
    out.write("MYENC001", 8);
    writeU32(out, 1);
    writeU32(out, static_cast<uint32_t>(salt.size()));
    writeU32(out, static_cast<uint32_t>(nonce.size()));
    writeU32(out, static_cast<uint32_t>(tag.size()));
    out.write(reinterpret_cast<const char*>(salt.data()), salt.size());
    out.write(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    out.write(reinterpret_cast<const char*>(cipher.data()), cipher.size());
    out.write(reinterpret_cast<const char*>(tag.data()), tag.size());
}

//解密
static void decryptFile(const fs::path& input, const fs::path& output, const std::string& password) {
    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open encrypted input");
    
    char magic[8];
    in.read(magic, 8);
    
    //检查是否是该系统加密的文件
    if (std::string(magic, 8) != "MYENC001") throw std::runtime_error("invalid encrypted file magic");
    
    (void)readU32(in);
    uint32_t saltLen = readU32(in), nonceLen = readU32(in), tagLen = readU32(in);
    std::vector<uint8_t> salt(saltLen), nonce(nonceLen);
    in.read(reinterpret_cast<char*>(salt.data()), salt.size());
    in.read(reinterpret_cast<char*>(nonce.data()), nonce.size());

    std::vector<uint8_t> rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (rest.size() < tagLen) throw std::runtime_error("encrypted file is truncated");
    std::vector<uint8_t> tag(rest.end() - tagLen, rest.end());
    std::vector<uint8_t> cipher(rest.begin(), rest.end() - tagLen);
    std::vector<uint8_t> key = pbkdf2(password, salt, 10000, 32);
    std::vector<uint8_t> tagData = salt;
    tagData.insert(tagData.end(), nonce.begin(), nonce.end());
    tagData.insert(tagData.end(), cipher.begin(), cipher.end());
    auto expectedArr = hmacSha256(key, tagData);
    std::vector<uint8_t> expected(expectedArr.begin(), expectedArr.end());

    if (!constantEqual(tag, expected)) throw std::runtime_error("password error or encrypted package is damaged");
    cryptStream(cipher, key, nonce);
    writeBinary(output, cipher);
}

//扩展名过滤
static bool endsWithAny(const fs::path& path, const std::vector<std::string>& exts) {
    if (exts.empty()) return true;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (std::string e : exts) {
        if (!e.empty() && e[0] != '.') e = "." + e;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (ext == e) return true;
    }
    return false;
}

//从过滤规则文件中加载规则
static FilterRule loadFilter(const std::string& filterPath) {
    FilterRule rule;
    if (filterPath.empty()) return rule;
    std::string text = readTextFile(filterPath);

    //从JSON文本中解析规则
    auto value = [&](const std::string& key) {
        auto pos = text.find("\"" + key + "\"");
        if (pos == std::string::npos) return std::string();
        pos = text.find(':', pos);
        if (pos == std::string::npos) return std::string();
        auto q1 = text.find('"', pos + 1);
        auto q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) return std::string();
        return text.substr(q1 + 1, q2 - q1 - 1);
    };

    //从JSON文本中解析数字
    auto number = [&](const std::string& key) -> uint64_t {
        auto pos = text.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        pos = text.find(':', pos);
        if (pos == std::string::npos) return 0;
        auto start = text.find_first_of("0123456789", pos + 1);
        if (start == std::string::npos) return 0;
        auto end = text.find_first_not_of("0123456789", start);
        return std::stoull(text.substr(start, end - start));// string to unsigned long long
    };

    std::string ext = value("extensions");
    std::stringstream ss(ext);
    for (std::string item; std::getline(ss, item, ',');) if (!item.empty()) rule.extensions.push_back(item);
    rule.nameContains = value("nameContains");
    rule.modifiedAfter = value("modifiedAfter");
    rule.minSize = number("minSize");
    rule.maxSize = number("maxSize");
    return rule;
}

//扫描文件，返回符合过滤规则的文件列表
static std::vector<FileEntry> scanFiles(const fs::path& source, const FilterRule& rule) {
    if (!fs::exists(source) || !fs::is_directory(source)) throw std::runtime_error("source directory does not exist");
    
    std::vector<FileEntry> files;
    
    //递归遍历源目录下的所有文件
    for (auto const& it : fs::recursive_directory_iterator(source)) {
        if (!it.is_regular_file()) continue;
        
        auto size = fs::file_size(it.path());
        
        //筛选规则
        if (!endsWithAny(it.path(), rule.extensions)) continue;
        if (!rule.nameContains.empty() && it.path().filename().string().find(rule.nameContains) == std::string::npos) continue;
        if (rule.minSize && size < rule.minSize) continue;
        if (rule.maxSize && size > rule.maxSize) continue;
        std::string modified = fileTimeText(it.path());
        if (!rule.modifiedAfter.empty() && modified < rule.modifiedAfter) continue;
        
        //生成manifest条目
        FileEntry e;
#ifdef _WIN32
        e.relativePath = wideToUtf8(fs::relative(it.path(), source).generic_wstring());
#else
        e.relativePath = fs::relative(it.path(), source).generic_string();
#endif
        e.size = size;
        e.length = size;
        e.modifiedTime = modified;
        e.sha256 = sha256File(it.path());
        
        files.push_back(e);
    }
    return files;
}

//生成manifest的JSON文本
static std::string manifestJson(const std::vector<FileEntry>& files) {
    std::ostringstream out;
    out << "{\n  \"backupId\":\"backup_" << std::time(nullptr) << "\",\n";
    out << "  \"createTime\":\"" << nowText() << "\",\n  \"formatVersion\":\"1.0\",\n  \"files\":[\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        out << "    {\"relativePath\":\"" << jsonEscape(f.relativePath) << "\",\"size\":" << f.size
            << ",\"sha256\":\"" << f.sha256 << "\",\"modifiedTime\":\"" << f.modifiedTime
            << "\",\"offset\":" << f.offset << ",\"length\":" << f.length << "}";
        if (i + 1 < files.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

//从JSON文本中获取字符串值
static std::string getJsonString(const std::string& obj, const std::string& key) {
    auto pos = obj.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    auto colon = obj.find(':', pos);
    auto q1 = obj.find('"', colon + 1);
    auto q2 = obj.find('"', q1 + 1);
    return jsonUnescape(obj.substr(q1 + 1, q2 - q1 - 1));
}

//从JSON文本中获取数字值
static uint64_t getJsonNumber(const std::string& obj, const std::string& key) {
    auto pos = obj.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    auto colon = obj.find(':', pos);
    auto start = obj.find_first_of("0123456789", colon + 1);
    auto end = obj.find_first_not_of("0123456789", start);
    return std::stoull(obj.substr(start, end - start));
}

//解析manifest
static std::vector<FileEntry> parseManifest(const std::string& manifest) {
    std::vector<FileEntry> files;
    size_t pos = manifest.find("\"files\"");
    while ((pos = manifest.find("{\"relativePath\"", pos)) != std::string::npos) {
        size_t end = manifest.find('}', pos);
        std::string obj = manifest.substr(pos, end - pos + 1);
        FileEntry f;
        f.relativePath = getJsonString(obj, "relativePath");
        f.size = getJsonNumber(obj, "size");
        f.sha256 = getJsonString(obj, "sha256");
        f.modifiedTime = getJsonString(obj, "modifiedTime");
        f.offset = getJsonNumber(obj, "offset");
        f.length = getJsonNumber(obj, "length");
        files.push_back(f);
        pos = end + 1;
    }
    return files;
}

//恢复路径安全，防止路径穿越攻击
static bool safeRelativePath(const std::string& path) {
    fs::path p(path);

    if (p.is_absolute()) return false;//拒绝绝对路径
    for (const auto& part : p) if (part == "..") return false;//拒绝上级目录

    return true;
}

//打包成bak文件
static void packBak(const fs::path& source, std::vector<FileEntry>& files, const fs::path& bak) {
   //offset和length可能会改变manifest的长度，导致manifest的长度变化，循环直到稳定
    std::string manifest = manifestJson(files);
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint64_t dataStart = 8 + 4 + 8 + manifest.size();//第一个文件起始位置
        uint64_t offset = dataStart;
        for (auto& f : files) {
            f.offset = offset;
            offset += f.length;
        }
        
        std::string next = manifestJson(files);
        if (next.size() == manifest.size()) {
            manifest = next;
            break;
        }
        manifest = next;
    }

    std::ofstream out(bak, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create bak file");
    
    //文件标识+版本号+manifest长度+manifest内容+文件数据
    out.write("MYBAK001", 8);
    writeU32(out, 1);
    writeU64(out, manifest.size());
    out.write(manifest.data(), static_cast<std::streamsize>(manifest.size()));

    //写入文件数据
    std::array<char, 64 * 1024> buf{};
    for (const auto& f : files) {
        std::ifstream in(source / pathFromUtf8(f.relativePath), std::ios::binary);//拼接得到绝对路径
        if (!in) throw std::runtime_error("cannot read source file: " + f.relativePath);
        while (in) {
            in.read(buf.data(), buf.size());
            if (in.gcount() > 0) out.write(buf.data(), in.gcount());
        }
    }
}

//从bak文件中读取manifest
static std::string readManifestFromBak(std::ifstream& in) {
    char magic[8];
    in.read(magic, 8);
    if (std::string(magic, 8) != "MYBAK001") throw std::runtime_error("invalid bak magic");
    (void)readU32(in);
    uint64_t manifestLen = readU64(in);
    std::string manifest(manifestLen, '\0');
    in.read(manifest.data(), static_cast<std::streamsize>(manifest.size()));
    return manifest;
}

//解包
static void restoreBak(const fs::path& bak, const fs::path& restoreDir, int& restored, int& hashFailed) {
    std::ifstream in(bak, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read bak");
    
    std::string manifest = readManifestFromBak(in);
    auto files = parseManifest(manifest);
    
    std::array<char, 64 * 1024> buf{};
    for (const auto& f : files) {
        if (!safeRelativePath(f.relativePath)) throw std::runtime_error("unsafe relative path in manifest: " + f.relativePath);
        //生成目标路径并创建目标目录
        fs::path target = restoreDir / pathFromUtf8(f.relativePath);
        fs::create_directories(target.parent_path());
        
        std::ofstream out(target, std::ios::binary);
        if (!out) throw std::runtime_error("cannot write restore file: " + pathToUtf8(target));
        in.clear();
        
        //定位到文件数据的起始位置
        in.seekg(static_cast<std::streamoff>(f.offset), std::ios::beg);
        uint64_t left = f.length;
        while (left > 0) {
            auto n = static_cast<std::streamsize>(std::min<uint64_t>(left, buf.size()));
            in.read(buf.data(), n);
            out.write(buf.data(), in.gcount());
            left -= static_cast<uint64_t>(in.gcount());
            if (in.gcount() == 0) throw std::runtime_error("bak file truncated");
        }
        out.close();
        ++restored;

        //校验文件哈希
        if (sha256File(target) != f.sha256) ++hashFailed;
    }
}

//解析命令行参数
static std::unordered_map<std::string, std::string> parseArgs(int argc, char** argv) {
    std::unordered_map<std::string, std::string> args;
    for (int i = 2; i < argc; ++i) {
        std::string key = argv[i];
        if (key.rfind("--", 0) == 0 && i + 1 < argc) {
            args[key.substr(2)] = argv[++i];
        }
    }
    return args;
}

static int runMain(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "usage: backup_core backup|restore --source DIR --output FILE --password PASS [--filter FILE]\n";
            return 2;
        }
        std::string mode = argv[1];
        auto args = parseArgs(argc, argv);

        //备份
        if (mode == "backup") {
            fs::path source = pathFromUtf8(args.at("source"));
            fs::path output = pathFromUtf8(args.at("output"));
            std::string password = args.at("password");

            //过滤规则
            FilterRule filter = loadFilter(args.count("filter") ? args["filter"] : "");
            
            //选择要备份的文件
            logLine("scan source directory");
            auto files = scanFiles(source, filter);
            
            if (files.empty()) throw std::runtime_error("no files matched backup rule");
            fs::create_directories(output.parent_path());
            fs::path bak = output;
            bak += ".tmp.bak";
            logLine("pack files: " + std::to_string(files.size()));
            
            //打包
            packBak(source, files, bak);
            logLine("encrypt package");
            
            //加密
            encryptFile(bak, output, password);
            
            fs::remove(bak);
            uint64_t total = 0;
            for (const auto& f : files) total += f.size;
            std::cout << "{\"success\":true,\"message\":\"backup success\",\"outputFile\":\"" << jsonEscape(pathToUtf8(output))
                      << "\",\"fileCount\":" << files.size() << ",\"totalSize\":" << total << "}" << std::endl;
            return 0;
        }

        //还原
        if (mode == "restore") {
            fs::path input = pathFromUtf8(args.at("input"));
            fs::path restore = pathFromUtf8(args.at("restore"));
            std::string password = args.at("password");

            //读取恢复目录
            fs::path bak = fs::temp_directory_path() / ("restore_" + std::to_string(std::time(nullptr)) + ".bak");
            
            //解密
            logLine("decrypt package");
            decryptFile(input, bak, password);
            
            //恢复文件
            logLine("restore files");
            int restored = 0, hashFailed = 0;
            restoreBak(bak, restore, restored, hashFailed);

            fs::remove(bak);
            std::cout << "{\"success\":true,\"message\":\"restore success\",\"restoredCount\":" << restored
                      << ",\"skippedCount\":0,\"hashFailedCount\":" << hashFailed << "}" << std::endl;
            return hashFailed == 0 ? 0 : 1;
        }
        throw std::runtime_error("unknown mode: " + mode);
    } catch (const std::exception& e) {
        std::cerr << "{\"success\":false,\"message\":\"" << jsonEscape(e.what()) << "\"}" << std::endl;
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    //将宽字符参数转换成UTF-8参数
    std::vector<std::string> utf8Args;
    std::vector<char*> argv;
    utf8Args.reserve(static_cast<size_t>(argc));
    argv.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        utf8Args.push_back(wideToUtf8(wargv[i]));
        argv.push_back(utf8Args.back().data());
    }
    return runMain(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    return runMain(argc, argv);
}
#endif
