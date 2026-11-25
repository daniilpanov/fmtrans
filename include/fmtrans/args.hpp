#pragma once

#include <map>
#include <string>
#include <vector>

namespace fmtrans {

struct Args {
  std::map<std::string, std::string> options;
  std::vector<std::string> positional;

  static Args parse(int argc, char** argv) {
    Args result;
    for (int i = 1; i < argc; ++i) {
      const std::string token = argv[i];
      if (token.rfind("--", 0) == 0) {
        const size_t eq = token.find('=');
        if (eq != std::string::npos) {
          result.options[token.substr(2, eq - 2)] = token.substr(eq + 1);
        } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
          result.options[token.substr(2)] = argv[++i];
        } else {
          result.options[token.substr(2)] = "";
        }
      } else {
        result.positional.push_back(token);
      }
    }
    return result;
  }

  bool has(const std::string& key) const { return options.count(key) > 0; }

  std::string get(const std::string& key, const std::string& fallback = "") const {
    const auto it = options.find(key);
    return it == options.end() ? fallback : it->second;
  }
};

}  // namespace fmtrans
