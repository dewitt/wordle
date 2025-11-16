#include "lookup_generator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>

#include "solver_runtime.h"

bool generate_lookup_table(const std::string &path,
                           const std::vector<encoded_word> &words,
                           encoded_word start, uint32_t depth,
                           const FeedbackTable *feedback_table,
                           const LookupTables &lookups) {
  if (depth < 1) {
    std::cerr << "Lookup depth must be at least 1.\n";
    return false;
  }
  const auto &weights = load_word_weights();

  constexpr uint32_t HEADER_SIZE = sizeof(LookupHeader);
  auto write_u32 = [](std::vector<uint8_t> &buf, uint32_t val) {
    const size_t pos = buf.size();
    buf.resize(pos + sizeof(uint32_t));
    std::memcpy(buf.data() + pos, &val, sizeof(uint32_t));
  };

  std::vector<uint8_t> buffer;
  buffer.reserve(1 << 20);

  std::function<uint32_t(const std::vector<size_t> &, encoded_word, uint32_t)>
      write_node = [&](const std::vector<size_t> &indices, encoded_word guess,
                       uint32_t remaining_depth) -> uint32_t {
    std::array<std::vector<size_t>, 243> partitions;
    for (const auto idx : indices) {
      const feedback_int fb = calculate_feedback_encoded(guess, words[idx]);
      partitions[fb].push_back(idx);
    }

    struct EntryData {
      uint16_t feedback;
      encoded_word next_guess;
      std::vector<size_t> subset;
    };
    std::vector<EntryData> entries;
    entries.reserve(243);

    for (uint16_t fb = 0; fb < 243; ++fb) {
      auto &subset = partitions[fb];
      if (subset.empty())
        continue;
      if (remaining_depth == 1) {
        size_t best_idx = subset[0];
        uint32_t best_weight = 0;
        for (const auto idx : subset) {
          const uint32_t weight = weights[idx];
          if (weight > best_weight) {
            best_weight = weight;
            best_idx = idx;
          }
        }
        encoded_word final_guess = words[best_idx];
        entries.push_back({fb, final_guess, std::move(subset)});
        continue;
      }
      encoded_word next = 0;
      if (subset.size() == 1) {
        entries.push_back({fb, words[subset[0]], {}});
        continue;
      }
      if (remaining_depth == 0) {
        entries.push_back({fb, 0, {}});
        continue;
      }
      if (feedback_table && feedback_table->loaded()) {
        next = find_best_guess_encoded(subset, words, false, guess, fb,
                                       feedback_table, lookups, weights);
      }
      if (next == 0) {
        next = find_best_guess_encoded(subset, words, false, guess, fb, nullptr,
                                       lookups, weights);
      }
      entries.push_back({fb, next, subset});
    }

    const uint32_t node_offset = static_cast<uint32_t>(buffer.size());
    write_u32(buffer, static_cast<uint32_t>(entries.size()));
    std::vector<size_t> child_positions;
    child_positions.reserve(entries.size());

    for (const auto &entry : entries) {
      const uint16_t fb = entry.feedback;
      const uint16_t reserved = 0;
      buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&fb),
                    reinterpret_cast<const uint8_t *>(&fb) + sizeof(fb));
      buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&reserved),
                    reinterpret_cast<const uint8_t *>(&reserved) +
                        sizeof(reserved));
      buffer.insert(buffer.end(),
                    reinterpret_cast<const uint8_t *>(&entry.next_guess),
                    reinterpret_cast<const uint8_t *>(&entry.next_guess) +
                        sizeof(entry.next_guess));
      child_positions.push_back(buffer.size());
      write_u32(buffer, 0);
    }

    for (size_t idx = 0; idx < entries.size(); ++idx) {
      const auto &entry = entries[idx];
      uint32_t child_offset = 0;
      if (remaining_depth > 1 && entry.subset.size() > 1 &&
          entry.next_guess != 0) {
        child_offset = HEADER_SIZE + write_node(entry.subset, entry.next_guess,
                                                remaining_depth - 1);
      }
      std::memcpy(buffer.data() + child_positions[idx], &child_offset,
                  sizeof(child_offset));
    }

    return node_offset;
  };

  std::vector<size_t> root_indices(words.size());
  std::iota(root_indices.begin(), root_indices.end(), 0);
  uint32_t root_offset =
      write_node(root_indices, start, depth > 0 ? depth - 1 : 0);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "Failed to open '" << path << "' for writing.\n";
    return false;
  }

  LookupHeader header{};
  std::memcpy(header.magic, "PLUT", 4);
  header.version = 1;
  header.depth = depth;
  header.root_offset = HEADER_SIZE + root_offset;
  header.start_encoded = start;
  std::string start_word = decode_word(start);
  std::memcpy(header.start_word, start_word.c_str(),
              std::min<size_t>(5, start_word.size()));

  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  out.write(reinterpret_cast<const char *>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
  std::cout << "Wrote lookup table '" << path << "' (" << buffer.size()
            << " bytes)\n";
  return true;
}
