//测试文件，需配置后运行
#include "server_utils.h"

#include <sstream>
#include <string>

bool safeName(const std::string& name) {
    return !name.empty()
        && name.find('/') == std::string::npos
        && name.find('\\') == std::string::npos
        && name.find("..") == std::string::npos;
}

std::string urlDecode(const std::string& s) {
    std::string out;

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(
                std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }

    return out;
}

std::string queryParam(const std::string& target, const std::string& key) {
    const auto q = target.find('?');
    if (q == std::string::npos) return "";

    const std::string query = target.substr(q + 1);
    std::stringstream ss(query);

    for (std::string item; std::getline(ss, item, '&');) {
        const auto eq = item.find('=');
        if (eq != std::string::npos && item.substr(0, eq) == key) {
            return urlDecode(item.substr(eq + 1));
        }
    }

    return "";
}
