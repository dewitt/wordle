#include "solver_runtime.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

bool PrecomputedLookup::load(const std::string &path,
                             encoded_word expected_start) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open())
    return false;
  const auto size = file.tellg();
  file.seekg(0);
  buffer_.resize(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char *>(buffer_.data()), size)) {
    buffer_.clear();
    return false;
  }
  if (buffer_.size() < sizeof(LookupHeader))
    return false;
  LookupHeader header{};
  std::memcpy(&header, buffer_.data(), sizeof(header));
  if (std::memcmp(header.magic, "PLUT", 4) != 0)
    return false;
  if (header.version != 1)
    return false;
  depth_ = header.depth;
  start_word_ = header.start_encoded;
  if (start_word_ != expected_start)
    return false;
  if (header.root_offset >= buffer_.size())
    return false;
  root_ptr_ = buffer_.data() + header.root_offset;
  return true;
}

const uint8_t *PrecomputedLookup::find_child(const uint8_t *node,
                                             uint16_t feedback,
                                             encoded_word &guess_out) const {
  if (!node)
    return nullptr;
  uint32_t count = *reinterpret_cast<const uint32_t *>(node);
  const uint8_t *ptr = node + 4;
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t fb = *reinterpret_cast<const uint16_t *>(ptr);
    ptr += 2;
    ptr += 2;
    encoded_word guess = *reinterpret_cast<const encoded_word *>(ptr);
    ptr += sizeof(encoded_word);
    uint32_t child = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += 4;
    if (fb == feedback) {
      guess_out = guess;
      if (child == 0)
        return nullptr;
      return buffer_.data() + child;
    }
  }
  return nullptr;
}

void run_non_interactive(encoded_word answer,
                         const std::vector<encoded_word> &words, bool hard_mode,
                         bool verbose, bool print_output, SolutionTrace *trace,
                         bool debug_lookup, const FeedbackTable *,
                         const LookupTables &, const PrecomputedLookup *tree) {
  (void)words;
  (void)hard_mode;
  if (!tree || !tree->root()) {
    std::cerr << "Error: precomputed lookup table is required for solving.\n";
    return;
  }

  const auto solve_start = std::chrono::high_resolution_clock::now();
  auto log_duration = [&](const char *tag) {
    if (!debug_lookup)
      return;
    const auto end = std::chrono::high_resolution_clock::now();
    const auto ms =
        std::chrono::duration<double, std::milli>(end - solve_start).count();
    std::cerr << "[timer] " << tag << " " << ms << " ms\n";
  };

  int turn = 1;
  encoded_word guess = kInitialGuess;
  const uint8_t *node = tree->root();

  if (verbose && print_output) {
    std::cout << "Solving for: " << decode_word(answer)
              << (hard_mode ? " (Hard Mode)" : "") << std::endl;
    std::cout << "------------------------------" << std::endl;
  }

  while (turn <= 6) {
    if (verbose && print_output) {
      std::cout << "Turn " << turn << std::endl;
    }

    feedback_int feedback_val = calculate_feedback_encoded(guess, answer);
    std::string feedback_str(5, '_');
    int temp = feedback_val;
    for (int i = 4; i >= 0; --i) {
      const int remainder = temp % 3;
      if (remainder == 2)
        feedback_str[i] = 'g';
      else if (remainder == 1)
        feedback_str[i] = 'y';
      temp /= 3;
    }

    if (verbose && print_output) {
      std::cout << "Guess: " << decode_word(guess)
                << ", Feedback: " << feedback_str << std::endl;
    }
    if (trace) {
      trace->steps.push_back({guess, feedback_val});
    }

    if (feedback_val == 242) {
      if (print_output) {
        std::cout << "\nSolved in " << turn << " guesses!" << std::endl;
      }
      log_duration("solved");
      return;
    }

    if (turn == 6) {
      std::cout << "Solver failed to find the word. Last guess was '"
                << decode_word(guess) << "'.\n";
      log_duration("failed-depth");
      return;
    }

    if (!node) {
      std::cout << "Solver failed: lookup table missing entries.\n";
      log_duration("failed-missing-node");
      return;
    }

    encoded_word next_guess = 0;
    const uint8_t *next_node =
        tree->find_child(node, static_cast<uint16_t>(feedback_val), next_guess);
    if (next_guess == 0) {
      std::cout << "Solver failed: lookup tree has no entry for feedback '"
                << feedback_str << "' on turn " << turn << ".\n";
      log_duration("failed-branch");
      return;
    }
    if (debug_lookup) {
      std::cerr << "[lookup] depth=" << (turn + 1)
                << " guess=" << decode_word(next_guess) << "\n";
    }
    guess = next_guess;
    node = next_node;
    ++turn;
  }

  std::cerr << "Solver failed to find the word. Last guess was '"
            << decode_word(guess) << "'.\n";
  log_duration("failed-depth");
}
