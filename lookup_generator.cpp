#include "lookup_generator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_set>
#include <vector>

#include "solver_core.h"
#include "solver_runtime.h"
#include "words_data.h"

namespace {

struct TreeEdge {
  uint16_t feedback = 0;
  encoded_word next_guess = 0;
  std::unique_ptr<struct TreeNode> child;
};

struct TreeNode {
  encoded_word guess = 0;
  std::vector<TreeEdge> edges;
};

struct ProgressStats {
  size_t states_completed = 0;
  size_t guesses_tried = 0;
  size_t backtracks = 0;
  uint32_t max_depth = 0;
  std::chrono::steady_clock::time_point last_log =
      std::chrono::steady_clock::now();
};

void log_progress(ProgressStats &stats, bool force = false) {
  auto now = std::chrono::steady_clock::now();
  if (!force && stats.states_completed == 0) {
    return;
  }
  if (!force && stats.states_completed % 100 != 0 &&
      now - stats.last_log < std::chrono::seconds(2)) {
    return;
  }
  stats.last_log = now;
  std::cerr << "\r[generate] states=" << stats.states_completed
            << " guesses=" << stats.guesses_tried
            << " backtracks=" << stats.backtracks
            << " max_depth=" << stats.max_depth << std::flush;
  if (force) {
    std::cerr << std::endl;
  }
}

void partition_indices(const std::vector<size_t> &indices, encoded_word guess,
                       const std::vector<encoded_word> &words,
                       const FeedbackTable *feedback_table,
                       const LookupTables &lookups,
                       std::array<std::vector<size_t>, 243> &partitions) {
  for (auto &bucket : partitions) {
    bucket.clear();
  }
  if (feedback_table && feedback_table->loaded()) {
    const auto it = lookups.word_index.find(guess);
    if (it != lookups.word_index.end()) {
      const uint8_t *row = feedback_table->row(it->second);
      for (const auto idx : indices) {
        partitions[row[idx]].push_back(idx);
      }
      return;
    }
  }
  for (const auto idx : indices) {
    const feedback_int fb =
        calculate_feedback_encoded(guess, words[idx]);
    partitions[fb].push_back(idx);
  }
}

bool build_subtree(TreeNode &node, const std::vector<size_t> &indices,
                   uint32_t depth_remaining, uint32_t total_depth,
                   const std::vector<encoded_word> &words,
                   const std::vector<uint32_t> &weights,
                   const FeedbackTable *feedback_table,
                   const LookupTables &lookups,
                   ProgressStats &stats, encoded_word forced_guess = 0) {
  if (indices.empty()) {
    return false;
  }
  if (indices.size() == 1) {
    node.guess = words[indices.front()];
    node.edges.clear();
    return true;
  }
  if (depth_remaining == 0) {
    return false;
  }

  const uint32_t current_depth = total_depth - depth_remaining + 1;
  stats.max_depth = std::max(stats.max_depth, current_depth);

  std::unordered_set<encoded_word> banned;
  bool use_forced = forced_guess != 0;

  while (true) {
    encoded_word guess = 0;
    if (use_forced) {
      guess = forced_guess;
      use_forced = false;
    } else {
      guess = find_best_guess_encoded(indices, words, feedback_table, lookups,
                                      weights, &banned);
    }

    if (guess == 0) {
      stats.backtracks++;
      return false;
    }

    if (!use_forced) {
      banned.insert(guess);
    }

    stats.guesses_tried++;
    node.guess = guess;
    std::array<std::vector<size_t>, 243> partitions;
    partition_indices(indices, guess, words, feedback_table, lookups,
                      partitions);

    bool success = true;
    std::vector<TreeEdge> edges;
    edges.reserve(243);

    for (uint16_t fb = 0; fb < 243; ++fb) {
      auto &subset = partitions[fb];
      if (subset.empty())
        continue;
      TreeEdge edge;
      edge.feedback = fb;
      if (subset.size() == 1) {
        edge.next_guess = words[subset[0]];
      } else {
        auto child = std::make_unique<TreeNode>();
        if (!build_subtree(*child, subset, depth_remaining - 1, total_depth,
                           words, weights, feedback_table, lookups, stats)) {
          success = false;
          break;
        }
        edge.next_guess = child->guess;
        edge.child = std::move(child);
      }
      edges.push_back(std::move(edge));
    }

    if (success) {
      std::sort(edges.begin(), edges.end(),
                [](const TreeEdge &a, const TreeEdge &b) {
                  return a.feedback < b.feedback;
                });
      node.edges = std::move(edges);
      stats.states_completed++;
      log_progress(stats);
      return true;
    }

    stats.backtracks++;
  }
}

uint32_t serialize_node(const TreeNode &node, std::vector<uint8_t> &buffer) {
  const uint32_t offset = static_cast<uint32_t>(buffer.size());
  const uint32_t count = static_cast<uint32_t>(node.edges.size());
  buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&count),
                reinterpret_cast<const uint8_t *>(&count) + sizeof(count));

  std::vector<size_t> child_positions;
  child_positions.reserve(node.edges.size());

  for (const auto &edge : node.edges) {
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&edge.feedback),
                  reinterpret_cast<const uint8_t *>(&edge.feedback) +
                      sizeof(edge.feedback));
    const uint16_t reserved = 0;
    buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&reserved),
                  reinterpret_cast<const uint8_t *>(&reserved) +
                      sizeof(reserved));
    buffer.insert(buffer.end(),
                  reinterpret_cast<const uint8_t *>(&edge.next_guess),
                  reinterpret_cast<const uint8_t *>(&edge.next_guess) +
                      sizeof(edge.next_guess));
    child_positions.push_back(buffer.size());
    const uint32_t placeholder = 0;
    buffer.insert(buffer.end(),
                  reinterpret_cast<const uint8_t *>(&placeholder),
                  reinterpret_cast<const uint8_t *>(&placeholder) +
                      sizeof(placeholder));
  }

  for (size_t i = 0; i < node.edges.size(); ++i) {
    if (node.edges[i].child) {
      const uint32_t child_offset =
          sizeof(LookupHeader) +
          serialize_node(*node.edges[i].child, buffer);
      std::memcpy(buffer.data() + child_positions[i], &child_offset,
                  sizeof(child_offset));
    }
  }

  return offset;
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

  const auto weights = compute_word_weights(words);
  TreeNode root;
  std::vector<size_t> root_indices(words.size());
  std::iota(root_indices.begin(), root_indices.end(), 0);

  ProgressStats stats;

  if (!build_subtree(root, root_indices, depth, depth, words, weights,
                     feedback_table, lookups, stats, start)) {
    std::cerr << "Failed to generate lookup table: depth limit too small.\n";
    return false;
  }

  std::vector<uint8_t> buffer;
  buffer.reserve(1 << 20);
  const uint32_t root_offset = serialize_node(root, buffer);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "Failed to open '" << path << "' for writing.\n";
    return false;
  }

  LookupHeader header{};
  std::memcpy(header.magic, "PLUT", 4);
  header.version = 1;
  header.depth = depth;
  header.root_offset = sizeof(LookupHeader) + root_offset;
  header.start_encoded = start;
  std::string start_word = decode_word(start);
  std::memcpy(header.start_word, start_word.c_str(),
              std::min<size_t>(5, start_word.size()));

  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  out.write(reinterpret_cast<const char *>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
  log_progress(stats, true);
  std::cout << "Wrote lookup table '" << path << "' (" << buffer.size()
            << " bytes, states=" << stats.states_completed
            << ", backtracks=" << stats.backtracks << ")\n";
  return true;
}
