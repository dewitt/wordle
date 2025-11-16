#include "words_data.h"

#include <array>
#include <cctype>
#include <fstream>
#include <iostream>

#include "word_lists.h"

std::string decode_word(encoded_word encoded) {
  std::string word = "     ";
  for (int i = 4; i >= 0; --i) {
    word[i] = static_cast<char>((encoded & 0x1F) + 'a' - 1);
    encoded >>= 5;
  }
  return word;
}

const std::vector<encoded_word> &load_words() {
  static const std::vector<encoded_word> words(std::begin(kEncodedWords),
                                               std::end(kEncodedWords));
  return words;
}

std::vector<encoded_word> load_words_from_file(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    std::cerr << "Failed to open word list '" << path << "'.\n";
    return {};
  }
  std::vector<encoded_word> words;
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() != 5)
      continue;
    for (char &c : line) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    words.push_back(encode_word(line));
  }
  if (words.empty()) {
    std::cerr << "No valid words found in '" << path << "'.\n";
  }
  return words;
}

std::vector<uint32_t>
compute_word_weights(const std::vector<encoded_word> &words) {
  std::array<uint32_t, 27> letter_counts{};
  for (const auto word : words) {
    for (int i = 0; i < 5; ++i) {
      const uint8_t code = get_char_code_at(word, i);
      letter_counts[code]++;
    }
  }
  std::vector<uint32_t> result(words.size());
  for (size_t idx = 0; idx < words.size(); ++idx) {
    encoded_word word = words[idx];
    std::array<bool, 27> seen{};
    uint32_t score = 0;
    for (int i = 0; i < 5; ++i) {
      const uint8_t code = get_char_code_at(word, i);
      if (!seen[code]) {
        score += letter_counts[code];
        seen[code] = true;
      }
    }
    result[idx] = score;
  }
  return result;
}

const std::vector<uint32_t> &load_word_weights() {
  static const std::vector<uint32_t> weights =
      compute_word_weights(load_words());
  return weights;
}
