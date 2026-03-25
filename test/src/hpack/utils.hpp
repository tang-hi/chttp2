#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
inline std::vector<uint8_t> hexToBytes(const std::string& hex) {
  std::vector<uint8_t> result;

  // Remove spaces from the input string
  std::string cleanHexString;
  std::copy_if(hex.begin(), hex.end(), std::back_inserter(cleanHexString), [](char c) {
    return !std::isspace(static_cast<unsigned char>(c));
  });

  // Remove "0x" prefix if present
  if (cleanHexString.substr(0, 2) == "0x") {
    cleanHexString = cleanHexString.substr(2);
  }

  // Ensure the string has an even number of characters
  if (cleanHexString.length() % 2 != 0) {
    cleanHexString = "0" + cleanHexString;
  }

  for (size_t i = 0; i < cleanHexString.length(); i += 2) {
    std::string byteString = cleanHexString.substr(i, 2);
    auto byte = static_cast<uint8_t>(std::stoi(byteString, nullptr, 16));
    result.push_back(byte);
  }

  return result;
}
