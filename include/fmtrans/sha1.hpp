#pragma once
#include <string>

namespace fmtrans {

// Returns the 20 raw digest bytes.
std::string sha1Raw(const std::string& input);

// Lower-case hex representation of sha1Raw.
std::string sha1Hex(const std::string& input);

std::string base64Encode(const std::string& input);

}  // namespace fmtrans
