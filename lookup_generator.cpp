#include "lookup_generator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
#include <unordered_map>

#include "solver_runtime.h"

namespace {

constexpr uint32_t kLookaheadDepth = 0;

class CandidateBitset {
public:
  CandidateBitset() = default;
  explicit CandidateBitset(size_t word_count)
      : bits_((word_count + 63) / 64, 0) {}

  static CandidateBitset FromIndices(const std::vector<size_t> &indices,
                                     size_t word_count) {
    CandidateBitset mask(word_count);
    for (const size_t idx : indices) {
      mask.set(idx);
    }
    return mask;
  }

  size_t count() const { return count_; }
  const std::vector<uint64_t> &bits() const { return bits_; }

private:
  void set(size_t idx) {
    const size_t bucket = idx / 64;
    const uint64_t bit = uint64_t{1} << (idx % 64);
    uint64_t &slot = bits_[bucket];
    if ((slot & bit) == 0) {
      slot |= bit;
      ++count_;
    }
  }

  std::vector<uint64_t> bits_;
  size_t count_ = 0;
};

struct MemoKey {
  std::vector<uint64_t> bits;
  uint32_t depth = 0;

  bool operator==(const MemoKey &other) const noexcept {
    return depth == other.depth && bits == other.bits;
  }
};

struct MemoKeyHash {
  size_t operator()(const MemoKey &key) const noexcept {
    size_t h = key.depth;
    for (const uint64_t word : key.bits) {
      h ^= std::hash<uint64_t>{}(word) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};

class GenerationMemo {
public:
  std::optional<uint32_t> find(const CandidateBitset &mask,
                               uint32_t depth) const {
    MemoKey probe;
    probe.depth = depth;
    probe.bits = mask.bits();
    const auto it = cache_.find(probe);
    if (it == cache_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void store(const CandidateBitset &mask, uint32_t depth, uint32_t offset) {
    MemoKey key;
    key.depth = depth;
    key.bits = mask.bits();
    cache_.emplace(std::move(key), offset);
  }

private:
  std::unordered_map<MemoKey, uint32_t, MemoKeyHash> cache_;
};

encoded_word choose_best_guess(const std::vector<size_t> &indices,
                               const std::vector<encoded_word> &words,
                               const FeedbackTable *feedback_table,
                               const LookupTables &lookups,
                               const std::vector<uint32_t> &weights,
                               uint32_t lookahead_depth) {
  (void)lookahead_depth; // Placeholder for deeper scoring.
  if (indices.empty())
    return 0;
  encoded_word best_guess = 0;
  size_t best_worst = std::numeric_limits<size_t>::max();
  size_t best_spread = std::numeric_limits<size_t>::max();
  int64_t best_weight = -1;

  std::array<size_t, 243> counts{};
  for (size_t guess_idx = 0; guess_idx < words.size(); ++guess_idx) {
    counts.fill(0);
    if (feedback_table && feedback_table->loaded()) {
      const uint8_t *row = feedback_table->row(guess_idx);
      for (const auto idx : indices) {
        counts[row[idx]]++;
      }
    } else {
      encoded_word guess = words[guess_idx];
      for (const auto idx : indices) {
        const feedback_int fb =
            calculate_feedback_encoded(guess, words[idx]);
        counts[fb]++;
      }
    }

    size_t worst = 0;
    size_t spread = 0;
    for (const auto cnt : counts) {
      if (cnt == 0)
        continue;
      worst = std::max(worst, cnt);
      spread += cnt * cnt;
    }

    const int64_t weight = weights[guess_idx];
    const auto candidate =
        std::make_tuple(worst, spread, -static_cast<int64_t>(weight));
    const auto current =
        std::make_tuple(best_worst, best_spread, -best_weight);
    if (!best_guess || candidate < current) {
      best_guess = words[guess_idx];
      best_worst = worst;
      best_spread = spread;
      best_weight = weight;
    }
  }

  return best_guess;
}

} // namespace

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

  const size_t word_count = words.size();
  GenerationMemo memo;

  std::function<uint32_t(const std::vector<size_t> &, const CandidateBitset &,
                         encoded_word, uint32_t)>
      write_node = [&](const std::vector<size_t> &indices,
                       const CandidateBitset &mask, encoded_word guess,
                       uint32_t remaining_depth) -> uint32_t {
    if (const auto cached = memo.find(mask, remaining_depth)) {
      return *cached;
    }

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
      const uint32_t lookahead =
          remaining_depth > 1
              ? std::min<uint32_t>(kLookaheadDepth, remaining_depth - 1)
              : 0;
      next = choose_best_guess(subset, words, feedback_table, lookups, weights,
                               lookahead);
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
        CandidateBitset child_mask =
            CandidateBitset::FromIndices(entry.subset, word_count);
        child_offset =
            HEADER_SIZE +
            write_node(entry.subset, child_mask, entry.next_guess,
                       remaining_depth - 1);
      }
      std::memcpy(buffer.data() + child_positions[idx], &child_offset,
                  sizeof(child_offset));
    }

    memo.store(mask, remaining_depth, node_offset);
    return node_offset;
  };

  std::vector<size_t> root_indices(words.size());
  std::iota(root_indices.begin(), root_indices.end(), 0);
  CandidateBitset root_mask =
      CandidateBitset::FromIndices(root_indices, word_count);
  uint32_t root_offset =
      write_node(root_indices, root_mask, start, depth > 0 ? depth - 1 : 0);

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
