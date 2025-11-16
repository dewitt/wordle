// (Existing functions: encode_word, decode_word, get_char_code_at,
// calculate_feedback_encoded, filter_word_list_encoded)

/**
 * solver.cpp - A highly optimized, multithreaded Wordle solver in C++.
 *
 * This program uses an entropy-reduction algorithm to find the best possible
 * Wordle guess. It has been optimized for performance through several
 * techniques:
 *
 * 1.  Integer Word Representation: 5-letter words are encoded into 64-bit
 *     unsigned integers (uint64_t), where each character is represented by 5
 *     bits. This makes comparisons and storage extremely fast and
 * cache-friendly.
 *
 * 2.  Bitwise Feedback Calculation: The core feedback logic operates directly
 * on these encoded integers using bitwise operations, avoiding costly string
 *     manipulations in the performance-critical sections.
 *
 * 3.  Multithreading: The search for the best guess is parallelized across all
 *     available CPU cores using std::async, making the calculation
 * significantly faster.
 *
 * 4.  Modern C++ Idioms: The code uses modern C++17 features like
 * std::string_view, std::async, and const correctness for safety, clarity, and
 * performance.
 *
 * It can be used to solve a specific Wordle puzzle or to run a one-off analysis
 * to find the single best starting word from scratch.
 */
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "word_lists.h"

// --- Type Definitions for Optimization ---
using encoded_word = uint64_t;
using feedback_int = int;

struct FeedbackTable {
  size_t guess_count = 0;
  size_t answer_count = 0;
  std::vector<uint8_t> owned_data;
  const uint8_t *mapped_data = nullptr;
  size_t mapping_length = 0;

  FeedbackTable() = default;
  FeedbackTable(const FeedbackTable &) = delete;
  FeedbackTable &operator=(const FeedbackTable &) = delete;
  FeedbackTable(FeedbackTable &&other) noexcept { *this = std::move(other); }
  FeedbackTable &operator=(FeedbackTable &&other) noexcept {
    if (this != &other) {
      release();
      guess_count = other.guess_count;
      answer_count = other.answer_count;
      owned_data = std::move(other.owned_data);
      mapped_data = other.mapped_data;
      mapping_length = other.mapping_length;
      other.mapped_data = nullptr;
      other.mapping_length = 0;
      other.guess_count = 0;
      other.answer_count = 0;
    }
    return *this;
  }

  ~FeedbackTable() { release(); }

  bool loaded() const { return mapped_data || !owned_data.empty(); }

  const uint8_t *data() const {
    return mapped_data ? mapped_data : owned_data.data();
  }

  const uint8_t *row(size_t guess_idx) const {
    return data() + guess_idx * answer_count;
  }

private:
  void release() {
#if defined(__unix__) || defined(__APPLE__)
    if (mapped_data) {
      munmap(const_cast<uint8_t *>(mapped_data), mapping_length);
    }
#endif
    mapped_data = nullptr;
    mapping_length = 0;
  }
};

struct LookupTables {
  std::unordered_map<encoded_word, size_t> word_index;
};

struct LookupHeader {
  char magic[4];
  uint32_t version;
  uint32_t depth;
  uint32_t root_offset;
  encoded_word start_encoded;
  char start_word[5];
  char reserved[3];
};
static_assert(sizeof(LookupHeader) == 32, "LookupHeader must be 32 bytes");

struct PrecomputedLookup {
  std::vector<uint8_t> buffer;
  const uint8_t *root = nullptr;
  uint32_t depth = 0;
  encoded_word start_word = 0;

  bool load(const std::string &path, encoded_word expected_start) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
      return false;
    const auto size = file.tellg();
    file.seekg(0);
    buffer.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(buffer.data()), size)) {
      buffer.clear();
      return false;
    }
    if (buffer.size() < sizeof(LookupHeader))
      return false;
    LookupHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    if (std::memcmp(header.magic, "PLUT", 4) != 0)
      return false;
    if (header.version != 1)
      return false;
    depth = header.depth;
    start_word = header.start_encoded;
    if (start_word != expected_start)
      return false;
    if (header.root_offset >= buffer.size())
      return false;
    root = buffer.data() + header.root_offset;
    return true;
  }

  const uint8_t *find_child(const uint8_t *node, uint16_t feedback,
                            encoded_word &guess_out,
                            bool &depth_limited) const {
    if (!node)
      return nullptr;
    uint32_t count = *reinterpret_cast<const uint32_t *>(node);
    const uint8_t *ptr = node + 4;
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t fb = *reinterpret_cast<const uint16_t *>(ptr);
      ptr += 2; // skip feedback
      uint16_t flags = *reinterpret_cast<const uint16_t *>(ptr);
      ptr += 2;
      encoded_word guess = *reinterpret_cast<const encoded_word *>(ptr);
      ptr += sizeof(encoded_word);
      uint32_t child = *reinterpret_cast<const uint32_t *>(ptr);
      ptr += 4;
      if (fb == feedback) {
        if (flags != 0) {
          depth_limited = true;
          guess_out = 0;
          return nullptr;
        }
        depth_limited = false;
        guess_out = guess;
        if (child == 0)
          return nullptr;
        return buffer.data() + child;
      }
    }
    depth_limited = false;
    return nullptr;
  }
};

// --- Encoding/Decoding Functions ---

// Encodes a 5-letter word into a 64-bit integer.
// Each character gets 5 bits (2^5=32, enough for 26 letters).
// 'a' -> 1, 'b' -> 2, ..., 'z' -> 26. 0 is reserved.
constexpr encoded_word encode_word(std::string_view word) {
  encoded_word encoded = 0;
  for (const char c : word) {
    encoded <<= 5;
    encoded |= (c - 'a' + 1);
  }
  return encoded;
}

// Decodes a 64-bit integer back into a 5-letter word.
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

constexpr encoded_word kInitialGuess = encode_word("roate");
constexpr const char *kFeedbackTablePath = "feedback_table.bin";

// --- Core Logic (Operating on Encoded Data) ---

// Helper to get the encoded character (1-26) at a specific position (0-4).
inline uint8_t get_char_code_at(encoded_word word, int pos) {
  return (word >> (5 * (4 - pos))) & 0x1F;
}

// Calculates feedback for two encoded words and returns an encoded feedback
// integer. This version operates purely on integers and bitwise operations.
feedback_int calculate_feedback_encoded(encoded_word guess_encoded,
                                        encoded_word answer_encoded) {
  uint8_t guess_codes[5];
  uint8_t answer_codes[5];
  uint8_t answer_counts[27] = {0}; // 1-26 for 'a'-'z'

  for (int i = 0; i < 5; ++i) {
    guess_codes[i] = get_char_code_at(guess_encoded, i);
    answer_codes[i] = get_char_code_at(answer_encoded, i);
    answer_counts[answer_codes[i]]++;
  }

  uint8_t feedback_codes[5] = {0}; // 0: gray, 1: yellow, 2: green

  // First pass for green letters (value 2)
  for (int i = 0; i < 5; ++i) {
    if (guess_codes[i] == answer_codes[i]) {
      feedback_codes[i] = 2;
      answer_counts[guess_codes[i]]--;
    }
  }

  // Second pass for yellow letters (value 1)
  for (int i = 0; i < 5; ++i) {
    if (feedback_codes[i] == 0 && answer_counts[guess_codes[i]] > 0) {
      feedback_codes[i] = 1;
      answer_counts[guess_codes[i]]--;
    }
  }

  // Convert feedback codes to a single integer (base-3 representation)
  feedback_int final_feedback = 0;
  for (int i = 0; i < 5; ++i) {
    final_feedback = final_feedback * 3 + feedback_codes[i];
  }
  return final_feedback;
}

LookupTables build_lookup_tables(const std::vector<encoded_word> &words) {
  LookupTables tables;
  tables.word_index.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    tables.word_index.emplace(words[i], i);
  }
  return tables;
}

const LookupTables &load_lookup_tables() {
  static const LookupTables tables = build_lookup_tables(load_words());
  return tables;
}

FeedbackTable load_feedback_table(const std::string &path, size_t word_count) {
  FeedbackTable table;
#if defined(__unix__) || defined(__APPLE__)
  const size_t expected_size = word_count * word_count;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    struct stat st{};
    if (fstat(fd, &st) == 0 &&
        static_cast<size_t>(st.st_size) == expected_size) {
      void *mapping = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapping != MAP_FAILED) {
        table.mapped_data = static_cast<const uint8_t *>(mapping);
        table.mapping_length = static_cast<size_t>(st.st_size);
        table.guess_count = word_count;
        table.answer_count = word_count;
        ::close(fd);
        return table;
      }
    }
    ::close(fd);
  }
#endif
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return table;
  }
  table.owned_data.resize(word_count * word_count);
  if (!file.read(reinterpret_cast<char *>(table.owned_data.data()),
                 static_cast<std::streamsize>(table.owned_data.size()))) {
    table.owned_data.clear();
    return table;
  }
  table.guess_count = word_count;
  table.answer_count = word_count;
  return table;
}

bool build_feedback_table_file(const std::string &path,
                               const std::vector<encoded_word> &words) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    std::cerr << "Failed to open '" << path << "' for writing.\n";
    return false;
  }
  size_t written = 0;
  for (const auto &guess : words) {
    for (const auto &answer : words) {
      const uint8_t feedback =
          static_cast<uint8_t>(calculate_feedback_encoded(guess, answer));
      file.put(static_cast<char>(feedback));
      ++written;
    }
  }
  file.flush();
  if (!file) {
    std::cerr << "Error writing feedback table to '" << path << "'.\n";
    return false;
  }
  std::cout << "Wrote " << written << " feedback entries to '" << path
            << "'.\n";
  return true;
}

// Filters a list of encoded words based on a guess and its feedback.
std::vector<size_t> filter_candidate_indices(
    const std::vector<size_t> &indices, encoded_word guess,
    feedback_int feedback, const FeedbackTable *feedback_table,
    const LookupTables &lookups, const std::vector<encoded_word> &words) {
  std::vector<size_t> new_indices;
  if (feedback_table && feedback_table->loaded()) {
    new_indices.reserve(indices.size());
    const auto guess_it = lookups.word_index.find(guess);
    if (guess_it == lookups.word_index.end()) {
      return new_indices;
    }
    const uint8_t *row = feedback_table->row(guess_it->second);
    for (const auto idx : indices) {
      if (row[idx] == feedback) {
        new_indices.push_back(idx);
      }
    }
    return new_indices;
  }

  new_indices.reserve(indices.size() / 2);
  for (const auto idx : indices) {
    if (calculate_feedback_encoded(guess, words[idx]) == feedback) {
      new_indices.push_back(idx);
    }
  }
  return new_indices;
}

// --- Hard Mode Logic ---

// Checks if a potential guess is valid under hard mode rules.
bool is_valid_hard_mode_guess(encoded_word potential_guess,
                              encoded_word previous_guess,
                              feedback_int previous_feedback) {
  uint8_t prev_guess_codes[5];
  uint8_t potential_guess_codes[5];
  uint8_t feedback_codes[5];

  int temp_feedback = previous_feedback;
  for (int i = 4; i >= 0; --i) {
    feedback_codes[i] = temp_feedback % 3;
    temp_feedback /= 3;
  }

  uint8_t required_yellows[27] = {0};

  for (int i = 0; i < 5; ++i) {
    prev_guess_codes[i] = get_char_code_at(previous_guess, i);
    potential_guess_codes[i] = get_char_code_at(potential_guess, i);

    // Rule 1: Green letters must be in the same spot.
    if (feedback_codes[i] == 2 &&
        potential_guess_codes[i] != prev_guess_codes[i]) {
      return false;
    }
    // Collect required yellow letters.
    if (feedback_codes[i] == 1) {
      required_yellows[prev_guess_codes[i]]++;
    }
  }

  // Rule 2: Yellow letters must be present in the new guess.
  uint8_t potential_guess_counts[27] = {0};
  for (int i = 0; i < 5; ++i) {
    potential_guess_counts[potential_guess_codes[i]]++;
  }

  for (int i = 1; i <= 26; ++i) {
    if (potential_guess_counts[i] < required_yellows[i]) {
      return false;
    }
  }

  return true;
}

// The worker task for std::async to find the best guess in a subset of words.
std::pair<encoded_word, double>
find_best_guess_worker(const std::vector<size_t> &possible_indices,
                       const std::vector<encoded_word> &guess_subset,
                       const FeedbackTable *feedback_table,
                       const LookupTables &lookups,
                       const std::vector<encoded_word> &words) {
  encoded_word local_best_guess = 0;
  double local_min_score = std::numeric_limits<double>::max();

  for (const auto &guess : guess_subset) {
    std::array<int, 243> feedback_groups{};
    double current_score = 0.0;
    bool pruned = false;
    if (feedback_table && feedback_table->loaded()) {
      const auto guess_it = lookups.word_index.find(guess);
      if (guess_it == lookups.word_index.end()) {
        continue;
      }
      const uint8_t *row = feedback_table->row(guess_it->second);
      for (const auto idx : possible_indices) {
        const uint8_t feedback = row[idx];
        const int count_before = feedback_groups[feedback];
        current_score += static_cast<double>(2 * count_before + 1);
        feedback_groups[feedback] = count_before + 1;
        if (current_score >= local_min_score) {
          pruned = true;
          break;
        }
      }
    } else {
      for (const auto idx : possible_indices) {
        const feedback_int feedback =
            calculate_feedback_encoded(guess, words[idx]);
        const int count_before = feedback_groups[feedback];
        current_score += static_cast<double>(2 * count_before + 1);
        feedback_groups[feedback] = count_before + 1;
        if (current_score >= local_min_score) {
          pruned = true;
          break;
        }
      }
    }

    if (!pruned && current_score < local_min_score) {
      local_min_score = current_score;
      local_best_guess = guess;
    }
  }
  return {local_best_guess, local_min_score};
}

// Finds the best word to guess next using a pool of asynchronous tasks.
encoded_word find_best_guess_encoded(
    const std::vector<size_t> &possible_indices,
    const std::vector<encoded_word> &words, bool hard_mode,
    encoded_word previous_guess, feedback_int previous_feedback,
    const FeedbackTable *feedback_table, const LookupTables &lookups) {
  if (possible_indices.empty()) {
    return 0;
  }

  // In hard mode, we must first filter the list of all possible guesses.
  const std::vector<encoded_word> *guesses_to_check = &words;
  std::vector<encoded_word> valid_hard_mode_guesses;
  if (hard_mode && previous_guess != 0) {
    valid_hard_mode_guesses.reserve(words.size() / 10);
    for (const auto &guess : words) {
      if (is_valid_hard_mode_guess(guess, previous_guess, previous_feedback)) {
        valid_hard_mode_guesses.push_back(guess);
      }
    }
    guesses_to_check = &valid_hard_mode_guesses;
  }

  unsigned int num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0)
    num_threads = 4;

  std::vector<std::vector<encoded_word>> word_chunks(num_threads);
  for (size_t i = 0; i < guesses_to_check->size(); ++i) {
    word_chunks[i % num_threads].push_back((*guesses_to_check)[i]);
  }

  std::vector<std::future<std::pair<encoded_word, double>>> futures;
  for (unsigned int i = 0; i < num_threads; ++i) {
    if (!word_chunks[i].empty()) {
      futures.push_back(std::async(std::launch::async, find_best_guess_worker,
                                   std::cref(possible_indices),
                                   std::cref(word_chunks[i]), feedback_table,
                                   std::cref(lookups), std::cref(words)));
    }
  }

  encoded_word best_guess = 0;
  double min_overall_score = std::numeric_limits<double>::max();

  for (auto &fut : futures) {
    auto result = fut.get();
    if (result.second < min_overall_score) {
      min_overall_score = result.second;
      best_guess = result.first;
    }
  }

  return best_guess;
}

// --- Main Application Logic ---

struct SolutionStep {
  encoded_word guess;
  feedback_int feedback;
};

struct SolutionTrace {
  std::vector<SolutionStep> steps;
};

void print_usage(const char *prog_name) {
  std::cout
      << "Usage:\n"
      << "  " << prog_name
      << " solve <word> [--hard-mode] [--debug] [--disable-lookup]\n"
      << "  " << prog_name << " start [--debug]\n"
      << "  " << prog_name
      << " generate [--lookup-depth N] [--lookup-output FILE]\n"
         "         [--lookup-start WORD] [--feedback-table]\n"
      << "  " << prog_name << " help\n\n"
      << "Flags:\n"
      << "  --debug           Verbose turn-by-turn output plus lookup/fallback "
         "diagnostics.\n"
      << "  --hard-mode       Enforce Wordle hard-mode constraints when "
         "solving.\n"
      << "  --disable-lookup  Ignore precomputed lookup tables (solve mode).\n"
      << "  --dump-json       Emit a JSON trace for solve mode instead of "
         "text.\n"
      << "  --lookup-depth N  Depth for lookup generation (default: 6).\n"
      << "  --lookup-output FILE  Output path for lookup table (default: "
         "lookup_<word>.bin).\n"
      << "  --lookup-start WORD   Start word when generating lookups "
         "(default: roate).\n"
      << "  --feedback-table  Rebuild feedback_table.bin before running.\n"
      << "  --help             Show this summary.\n";
}

void run_non_interactive(encoded_word answer,
                         const std::vector<encoded_word> &words, bool hard_mode,
                         bool verbose, bool print_output, SolutionTrace *trace,
                         bool debug_lookup, const FeedbackTable *feedback_table,
                         const LookupTables &lookups,
                         const PrecomputedLookup *precomputed) {
  std::vector<size_t> possible_indices(words.size());
  std::iota(possible_indices.begin(), possible_indices.end(), 0);
  int turn = 1;
  encoded_word guess = 0;
  feedback_int feedback_val = 0;
  const uint8_t *lookup_node = precomputed ? precomputed->root : nullptr;
  const uint32_t lookup_max =
      (precomputed && precomputed->depth > 0) ? (precomputed->depth - 1) : 0;
  uint32_t lookup_level = 0;

  if (verbose && print_output) {
    std::cout << "Solving for: " << decode_word(answer)
              << (hard_mode ? " (Hard Mode)" : "") << std::endl;
    std::cout << "------------------------------" << std::endl;
  }

  while (turn <= 6 && guess != answer) {
    if (verbose && print_output) {
      std::cout << "Turn " << turn << " (" << possible_indices.size()
                << " possibilities remain)" << std::endl;
    }

    bool used_lookup = false;
    if (!hard_mode && precomputed && lookup_node && turn > 1 &&
        lookup_level < lookup_max) {
      encoded_word lookup_guess = 0;
      bool depth_limited = false;
      const uint8_t *next_node = precomputed->find_child(
          lookup_node, static_cast<uint16_t>(feedback_val), lookup_guess,
          depth_limited);
      if (!depth_limited && lookup_guess != 0) {
        if (debug_lookup) {
          std::cerr << "[lookup] depth=" << turn << " fb=" << feedback_val
                    << " guess=" << decode_word(lookup_guess) << "\n";
        }
        guess = lookup_guess;
        lookup_node = next_node;
        lookup_level++;
        used_lookup = true;
      } else if (depth_limited) {
        lookup_node = nullptr;
      } else {
        lookup_node = nullptr;
      }
    }

    if (!used_lookup) {
      if (turn == 1) {
        guess = kInitialGuess;
      } else if (possible_indices.size() == 1) {
        guess = words[possible_indices[0]];
      } else {
        if (debug_lookup) {
          std::cerr << "[fallback] depth=" << turn << " fb=" << feedback_val
                    << " subset=" << possible_indices.size() << "\n";
        }
        guess =
            find_best_guess_encoded(possible_indices, words, hard_mode, guess,
                                    feedback_val, feedback_table, lookups);
        lookup_node = nullptr;
      }
    }

    if (guess == 0) {
      std::cout << "Solver failed to find a valid guess." << std::endl;
      break;
    }

    feedback_val = calculate_feedback_encoded(guess, answer);

    std::string feedback_str;
    feedback_str.reserve(5);
    int temp_feedback = feedback_val;
    for (int i = 0; i < 5; ++i) {
      const int remainder = temp_feedback % 3;
      if (remainder == 2)
        feedback_str += 'g';
      else if (remainder == 1)
        feedback_str += 'y';
      else
        feedback_str += '_';
      temp_feedback /= 3;
    }
    std::reverse(feedback_str.begin(), feedback_str.end());

    if (verbose && print_output) {
      std::cout << "Guess: " << decode_word(guess)
                << ", Feedback: " << feedback_str << std::endl;
    }

    if (trace) {
      trace->steps.push_back({guess, feedback_val});
    }

    if (feedback_str == "ggggg") {
      if (print_output) {
        std::cout << "\nSolved in " << turn << " guesses!" << std::endl;
      }
      return;
    }

    possible_indices = filter_candidate_indices(
        possible_indices, guess, feedback_val, feedback_table, lookups, words);
    turn++;
  }

  if (guess != answer && print_output) {
    std::cout << "\nSolver failed to find the word. Last guess was '"
              << decode_word(guess) << "'." << std::endl;
  }
}

bool generate_lookup_table(const std::string &path,
                           const std::vector<encoded_word> &words,
                           encoded_word start, uint32_t depth,
                           const FeedbackTable *feedback_table,
                           const LookupTables &lookups);

int main(int argc, char *argv[]) {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);

  bool debug_flag = false;
  bool hard_mode = false;
  bool dump_json = false;
  bool disable_lookup = false;
  bool rebuild_feedback_table = false;
  uint32_t lookup_depth = 6;
  std::string lookup_output;
  encoded_word lookup_start = kInitialGuess;

  std::string mode;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--debug") {
      debug_flag = true;
      continue;
    }
    if (arg == "--hard-mode") {
      hard_mode = true;
      continue;
    }
    if (arg == "--disable-lookup") {
      disable_lookup = true;
      continue;
    }
    if (arg == "--dump-json") {
      dump_json = true;
      continue;
    }
    if (arg == "--lookup-depth") {
      if (i + 1 >= argc) {
        std::cerr << "--lookup-depth requires a value.\n";
        return 1;
      }
      lookup_depth = static_cast<uint32_t>(std::stoul(argv[++i]));
      continue;
    }
    if (arg == "--lookup-output") {
      if (i + 1 >= argc) {
        std::cerr << "--lookup-output requires a path.\n";
        return 1;
      }
      lookup_output = argv[++i];
      continue;
    }
    if (arg == "--lookup-start") {
      if (i + 1 >= argc) {
        std::cerr << "--lookup-start requires a word.\n";
        return 1;
      }
      std::string start_arg = argv[++i];
      if (start_arg.size() != 5) {
        std::cerr << "--lookup-start requires a 5-letter word.\n";
        return 1;
      }
      lookup_start = encode_word(start_arg);
      continue;
    }
    if (arg == "--feedback-table") {
      rebuild_feedback_table = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Unknown flag: " << arg << "\n";
      return 1;
    }
    if (mode.empty()) {
      mode = arg;
    } else {
      positional.push_back(arg);
    }
  }

  if (mode.empty()) {
    std::cerr << "No mode specified.\n";
    print_usage(argv[0]);
    return 1;
  }

  std::string normalized_mode = mode;
  std::transform(normalized_mode.begin(), normalized_mode.end(),
                 normalized_mode.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (normalized_mode == "help") {
    print_usage(argv[0]);
    return 0;
  }

  const bool solve_mode = normalized_mode == "solve";
  const bool start_mode = normalized_mode == "start";
  const bool generate_mode = normalized_mode == "generate";

  if (!solve_mode && !start_mode && !generate_mode) {
    std::cerr << "Unknown mode '" << mode << "'.\n";
    print_usage(argv[0]);
    return 1;
  }

  if (dump_json && !solve_mode) {
    std::cerr << "--dump-json is only valid in solve mode.\n";
    return 1;
  }

  std::string word_to_solve;
  if (solve_mode) {
    if (!positional.empty()) {
      word_to_solve = positional.front();
      positional.erase(positional.begin());
    } else {
      std::cerr << "solve mode requires a target word.\n";
      return 1;
    }
    if (!positional.empty()) {
      std::cerr << "Unexpected extra arguments for solve mode.\n";
      return 1;
    }
  } else if (start_mode) {
    if (!positional.empty()) {
      std::cerr << "start mode does not take positional arguments.\n";
      return 1;
    }
  } else if (generate_mode) {
    if (!positional.empty()) {
      if (lookup_output.empty() && positional.size() == 1) {
        lookup_output = positional.front();
      } else {
        std::cerr << "Unexpected positional arguments for generate mode.\n";
        return 1;
      }
    }
  }

  const auto &words = load_words();
  const LookupTables &lookups = load_lookup_tables();

  if (words.empty()) {
    std::cerr << "Embedded word list is empty. Exiting.\n";
    return 1;
  }

  if (rebuild_feedback_table) {
    if (!build_feedback_table_file(kFeedbackTablePath, words)) {
      return 1;
    }
  }

  FeedbackTable feedback_table =
      load_feedback_table(kFeedbackTablePath, words.size());
  const FeedbackTable *feedback_ptr =
      feedback_table.loaded() ? &feedback_table : nullptr;

  if (!feedback_ptr) {
    std::cerr << "Warning: feedback table not found at '" << kFeedbackTablePath
              << "'. Falling back to slower feedback calculation.\n";
  }

  if (generate_mode) {
    if (!lookups.word_index.count(lookup_start)) {
      std::cerr << "Lookup start word must be in the allowed guess list.\n";
      return 1;
    }
    if (lookup_output.empty()) {
      lookup_output = "lookup_" + decode_word(lookup_start) + ".bin";
    }
    if (!generate_lookup_table(lookup_output, words, lookup_start, lookup_depth,
                               feedback_ptr, lookups)) {
      return 1;
    }
    return 0;
  }

  PrecomputedLookup lookup_table;
  const PrecomputedLookup *lookup_ptr = nullptr;
  if (!hard_mode && !disable_lookup &&
      lookup_table.load("lookup_roate.bin", kInitialGuess)) {
    lookup_ptr = &lookup_table;
  }

  if (start_mode) {
    std::vector<size_t> indices(words.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::cout << "Calculating the best starting word across " << words.size()
              << " valid words..." << std::endl;

    const auto start_time = std::chrono::high_resolution_clock::now();
    const encoded_word best_word = find_best_guess_encoded(
        indices, words, false, 0, 0, feedback_ptr, lookups);
    const auto end_time = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "\nBest starting word: " << decode_word(best_word)
              << "\nCalculation time: " << elapsed.count() << " seconds.\n";
    return 0;
  }

  const encoded_word encoded_answer = encode_word(word_to_solve);
  if (!lookups.word_index.count(encoded_answer)) {
    std::cerr << "Error: '" << word_to_solve
              << "' is not in the valid word list.\n";
    return 1;
  }

  const bool verbose_output = debug_flag;
  const bool debug_lookup = debug_flag;

  SolutionTrace trace;
  run_non_interactive(encoded_answer, words, hard_mode, verbose_output,
                      !dump_json, &trace, debug_lookup, feedback_ptr, lookups,
                      lookup_ptr);

  if (dump_json) {
    std::cout << "[";
    for (size_t i = 0; i < trace.steps.size(); ++i) {
      const auto &step = trace.steps[i];
      std::cout << "{\"guess\":\"" << decode_word(step.guess)
                << "\",\"feedback\":" << step.feedback << "}";
      if (i + 1 < trace.steps.size())
        std::cout << ",";
    }
    std::cout << "]\n";
  } else if (!verbose_output) {
    for (const auto &step : trace.steps) {
      std::cout << decode_word(step.guess) << ' ';
    }
    std::cout << "\n";
  }

  return 0;
}
bool generate_lookup_table(const std::string &path,
                           const std::vector<encoded_word> &words,
                           encoded_word start, uint32_t depth,
                           const FeedbackTable *feedback_table,
                           const LookupTables &lookups) {
  if (depth < 2) {
    std::cerr << "Lookup depth must be at least 2.\n";
    return false;
  }

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
      encoded_word next = 0;
      if (subset.size() == 1) {
        next = words[subset[0]];
      } else if (feedback_table && feedback_table->loaded()) {
        next = find_best_guess_encoded(subset, words, false, guess, fb,
                                       feedback_table, lookups);
      }
      if (next == 0) {
        next = find_best_guess_encoded(subset, words, false, guess, fb, nullptr,
                                       lookups);
      }
      entries.push_back({fb, next, std::move(subset)});
    }

    const uint32_t node_offset = static_cast<uint32_t>(buffer.size());
    write_u32(buffer, static_cast<uint32_t>(entries.size()));
    std::vector<size_t> child_positions;
    child_positions.reserve(entries.size());

    for (const auto &entry : entries) {
      const uint16_t fb = entry.feedback;
      const uint16_t flags =
          (remaining_depth <= 1 && entry.subset.size() > 1) ? 1 : 0;
      buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&fb),
                    reinterpret_cast<const uint8_t *>(&fb) + sizeof(fb));
      buffer.insert(buffer.end(), reinterpret_cast<const uint8_t *>(&flags),
                    reinterpret_cast<const uint8_t *>(&flags) +
                        sizeof(flags));
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
      if (remaining_depth > 1 && entry.subset.size() > 1) {
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
  uint32_t root_offset = write_node(root_indices, start, depth);

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
