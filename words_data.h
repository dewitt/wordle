#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "solver_types.h"

// Encodes a 5-letter word into a 64-bit integer with 5 bits per letter.
constexpr encoded_word encode_word(std::string_view word) {
  encoded_word encoded = 0;
  for (const char c : word) {
    encoded <<= 5;
    encoded |= (c - 'a' + 1);
  }
  return encoded;
}

// Helper to extract the encoded letter (1-26) at position [0,4].
inline uint8_t get_char_code_at(encoded_word word, int pos) {
  return (word >> (5 * (4 - pos))) & 0x1F;
}

std::string decode_word(encoded_word encoded);
const std::vector<encoded_word> &load_words();
const std::vector<uint32_t> &load_word_weights();

inline constexpr encoded_word kInitialGuess = encode_word("roate");
inline constexpr std::string_view kFeedbackTablePath = "feedback_table.bin";
