//测试文件，需配置后运行
#pragma once

#include <string>

// 检查文件名是否安全，防止空文件名和路径穿越。
bool safeName(const std::string& name);

// 解码 URL 中的 %XX 和加号。
std::string urlDecode(const std::string& s);

// 从 URL 查询字符串中取得指定参数。
std::string queryParam(const std::string& target, const std::string& key);
