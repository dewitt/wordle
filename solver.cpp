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
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
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
#include "opening_table.h"
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
  std::unordered_map<encoded_word, size_t> answer_index;
  std::unordered_map<encoded_word, size_t> guess_index;
};

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
    if (size < 32)
      return false;
    const uint8_t *ptr = buffer.data();
    if (std::memcmp(ptr, "PLUT", 4) != 0)
      return false;
    ptr += 4;
    const uint32_t version = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += 4;
    depth = *reinterpret_cast<const uint32_t *>(ptr);
    ptr += 4;
    start_word = *reinterpret_cast<const encoded_word *>(ptr);
    ptr += sizeof(encoded_word);
    ptr += 5; // start word string
    ptr += 3; // padding
    const uint32_t root_offset = *reinterpret_cast<const uint32_t *>(ptr);
    if (version != 1 || start_word != expected_start ||
        root_offset >= buffer.size())
      return false;
    root = buffer.data() + root_offset;
    return true;
  }

  const uint8_t *find_child(const uint8_t *node, uint16_t feedback,
                            encoded_word &guess_out) const {
    if (!node)
      return nullptr;
    uint32_t count = *reinterpret_cast<const uint32_t *>(node);
    const uint8_t *ptr = node + 4;
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t fb = *reinterpret_cast<const uint16_t *>(ptr);
      ptr += 2; // skip feedback
      ptr += 2; // skip reserved
      encoded_word guess = *reinterpret_cast<const encoded_word *>(ptr);
      ptr += sizeof(encoded_word);
      uint32_t child = *reinterpret_cast<const uint32_t *>(ptr);
      ptr += 4;
      if (fb == feedback) {
        guess_out = guess;
        if (child == 0)
          return nullptr;
        return buffer.data() + child;
      }
    }
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

const std::vector<encoded_word> &load_answers() {
  static const std::vector<encoded_word> answers(std::begin(kEncodedAnswers),
                                                 std::end(kEncodedAnswers));
  return answers;
}

const std::vector<encoded_word> &load_guesses() {
  static const std::vector<encoded_word> guesses(std::begin(kEncodedGuesses),
                                                 std::end(kEncodedGuesses));
  return guesses;
}

const std::vector<encoded_word> &load_all_words() {
  static const std::vector<encoded_word> combined = [] {
    std::vector<encoded_word> words;
    words.reserve(kAnswersCount + kGuessesCount);
    words.insert(words.end(), kEncodedAnswers, kEncodedAnswers + kAnswersCount);
    words.insert(words.end(), kEncodedGuesses, kEncodedGuesses + kGuessesCount);
    return words;
  }();
  return combined;
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

LookupTables build_lookup_tables(const std::vector<encoded_word> &answers,
                                 const std::vector<encoded_word> &all_words) {
  LookupTables tables;
  tables.answer_index.reserve(answers.size());
  for (size_t i = 0; i < answers.size(); ++i) {
    tables.answer_index.emplace(answers[i], i);
  }
  tables.guess_index.reserve(all_words.size());
  for (size_t i = 0; i < all_words.size(); ++i) {
    tables.guess_index.emplace(all_words[i], i);
  }
  return tables;
}

const LookupTables &load_lookup_tables() {
  static const LookupTables tables =
      build_lookup_tables(load_answers(), load_all_words());
  return tables;
}

FeedbackTable load_feedback_table(const std::string &path, size_t guess_count,
                                  size_t answer_count) {
  FeedbackTable table;
#if defined(__unix__) || defined(__APPLE__)
  const size_t expected_size = guess_count * answer_count;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    struct stat st{};
    if (fstat(fd, &st) == 0 &&
        static_cast<size_t>(st.st_size) == expected_size) {
      void *mapping = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapping != MAP_FAILED) {
        table.mapped_data = static_cast<const uint8_t *>(mapping);
        table.mapping_length = static_cast<size_t>(st.st_size);
        table.guess_count = guess_count;
        table.answer_count = answer_count;
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
  table.owned_data.resize(guess_count * answer_count);
  if (!file.read(reinterpret_cast<char *>(table.owned_data.data()),
                 static_cast<std::streamsize>(table.owned_data.size()))) {
    table.owned_data.clear();
    return table;
  }
  table.guess_count = guess_count;
  table.answer_count = answer_count;
  return table;
}

bool build_feedback_table_file(const std::string &path,
                               const std::vector<encoded_word> &answers,
                               const std::vector<encoded_word> &all_words) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    std::cerr << "Failed to open '" << path << "' for writing.\n";
    return false;
  }
  size_t written = 0;
  for (const auto &guess : all_words) {
    for (const auto &answer : answers) {
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
    const LookupTables &lookups, const std::vector<encoded_word> &answers) {
  std::vector<size_t> new_indices;
  if (feedback_table && feedback_table->loaded()) {
    new_indices.reserve(indices.size());
    const auto guess_it = lookups.guess_index.find(guess);
    if (guess_it == lookups.guess_index.end()) {
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
    if (calculate_feedback_encoded(guess, answers[idx]) == feedback) {
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
                       const std::vector<encoded_word> &answers) {
  encoded_word local_best_guess = 0;
  double local_min_score = std::numeric_limits<double>::max();

  for (const auto &guess : guess_subset) {
    std::array<int, 243> feedback_groups{};
    double current_score = 0.0;
    bool pruned = false;
    if (feedback_table && feedback_table->loaded()) {
      const auto guess_it = lookups.guess_index.find(guess);
      if (guess_it == lookups.guess_index.end()) {
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
            calculate_feedback_encoded(guess, answers[idx]);
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
    const std::vector<encoded_word> &answers,
    const std::vector<encoded_word> &all_words, bool hard_mode,
    encoded_word previous_guess, feedback_int previous_feedback,
    const FeedbackTable *feedback_table, const LookupTables &lookups) {
  if (possible_indices.empty()) {
    return 0;
  }

  // In hard mode, we must first filter the list of all possible guesses.
  const std::vector<encoded_word> *guesses_to_check = &all_words;
  std::vector<encoded_word> valid_hard_mode_guesses;
  if (hard_mode && previous_guess != 0) {
    valid_hard_mode_guesses.reserve(all_words.size() / 10);
    for (const auto &guess : all_words) {
      if (is_valid_hard_mode_guess(guess, previous_guess, previous_feedback)) {
        valid_hard_mode_guesses.push_back(guess);
      }
    }
    guesses_to_check = &valid_hard_mode_guesses;
  }

  constexpr size_t kAnswerOnlyThreshold = 1024;
  std::vector<encoded_word> limited_guesses;
  if (possible_indices.size() <= kAnswerOnlyThreshold) {
    limited_guesses.reserve(possible_indices.size());
    for (const auto idx : possible_indices) {
      limited_guesses.push_back(answers[idx]);
    }
    guesses_to_check = &limited_guesses;
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
                                   std::cref(lookups), std::cref(answers)));
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

void run_non_interactive(
    encoded_word answer, const std::vector<encoded_word> &answers,
    const std::vector<encoded_word> &all_words, bool hard_mode, bool verbose,
    bool print_output, SolutionTrace *trace,
    const FeedbackTable *feedback_table, const LookupTables &lookups,
    const PrecomputedLookup *precomputed) {
  std::vector<size_t> possible_indices(answers.size());
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
      const uint8_t *next_node =
          precomputed->find_child(lookup_node,
                                  static_cast<uint16_t>(feedback_val),
                                  lookup_guess);
      if (lookup_guess != 0) {
        guess = lookup_guess;
        lookup_node = next_node;
        lookup_level++;
        used_lookup = true;
      } else {
        lookup_node = nullptr;
      }
    }

    if (!used_lookup) {
      if (turn == 1) {
        guess = kInitialGuess;
      } else if (!hard_mode && turn == 2) {
        const encoded_word precomputed_guess = kSecondGuessTable[feedback_val];
        if (precomputed_guess != 0) {
          guess = precomputed_guess;
          lookup_node = nullptr;
        } else {
          guess = find_best_guess_encoded(possible_indices, answers, all_words,
                                          hard_mode, guess, feedback_val,
                                          feedback_table, lookups);
          lookup_node = nullptr;
        }
      } else if (possible_indices.size() == 1) {
        guess = answers[possible_indices[0]];
      } else {
        guess = find_best_guess_encoded(possible_indices, answers, all_words,
                                        hard_mode, guess, feedback_val,
                                        feedback_table, lookups);
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

    possible_indices =
        filter_candidate_indices(possible_indices, guess, feedback_val,
                                 feedback_table, lookups, answers);
    turn++;
  }

  if (guess != answer && print_output) {
    std::cout << "\nSolver failed to find the word. Last guess was '"
              << decode_word(guess) << "'." << std::endl;
  }
}

int main(int argc, char *argv[]) {
  // Fast I/O
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(NULL);

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " [--verbose] --word <target_word> [--hard-mode]\n";
    std::cerr << "   or: " << argv[0] << " [--verbose] --find-best-start\n";
    std::cerr << "   or: " << argv[0] << " --build-feedback-table\n";
    return 1;
  }

  bool verbose = false;
  bool hard_mode = false;
  bool dump_json = false;
  bool disable_lookup = false;
  std::string mode;
  std::string word_to_solve;

  for (int i = 1; i < argc;) {
    std::string arg = argv[i];
    if (arg == "--verbose") {
      verbose = true;
      ++i;
      continue;
    }
    if (arg == "--hard-mode") {
      hard_mode = true;
      ++i;
      continue;
    }
    if (arg == "--dump-json") {
      dump_json = true;
      ++i;
      continue;
    }
    if (arg == "--disable-lookup") {
      disable_lookup = true;
      ++i;
      continue;
    }
    if (arg == "--word") {
      if (!mode.empty()) {
        std::cerr << "--word cannot be combined with other modes.\n";
        return 1;
      }
      mode = "--word";
      if (i + 1 >= argc) {
        std::cerr << "Error: --word requires a target word.\n";
        return 1;
      }
      word_to_solve = argv[i + 1];
      i += 2;
      continue;
    }
    if (arg == "--find-best-start" || arg == "--build-feedback-table") {
      if (!mode.empty()) {
        std::cerr << "Multiple modes specified.\n";
        return 1;
      }
      mode = arg;
      ++i;
      continue;
    }
    std::cerr << "Unknown argument: " << arg << std::endl;
    return 1;
  }

  if (mode.empty()) {
    std::cerr << "No mode specified.\n";
    return 1;
  }

  if (mode == "--word" && word_to_solve.empty()) {
    std::cerr << "Error: --word requires a target word.\n";
    return 1;
  }

  const auto &answers = load_answers();
  const auto &all_words = load_all_words();
  const LookupTables &lookups = load_lookup_tables();

  if (answers.empty()) {
    std::cerr << "Embedded answers list is empty. Exiting." << std::endl;
    return 1;
  }

  const size_t answers_count = answers.size();
  std::vector<size_t> initial_indices(answers_count);
  std::iota(initial_indices.begin(), initial_indices.end(), 0);
  FeedbackTable feedback_table;
  const FeedbackTable *feedback_ptr = nullptr;

  if (mode == "--build-feedback-table") {
    if (!build_feedback_table_file(kFeedbackTablePath, answers, all_words)) {
      return 1;
    }
    return 0;
  }

  feedback_table =
      load_feedback_table(kFeedbackTablePath, all_words.size(), answers_count);
  if (feedback_table.loaded()) {
    feedback_ptr = &feedback_table;
  }
  PrecomputedLookup lookup_table;
  const PrecomputedLookup *lookup_ptr = nullptr;
  if (!hard_mode && !disable_lookup &&
      lookup_table.load("lookup_roate.bin", kInitialGuess)) {
    lookup_ptr = &lookup_table;
  }

  if (!feedback_ptr) {
    std::cerr << "Warning: feedback table not found at '" << kFeedbackTablePath
              << "'. Falling back to slower feedback calculation.\n";
  }

  if (mode == "--find-best-start") {
    std::cout << "Calculating the best starting word from " << all_words.size()
              << " guesses against " << answers_count << " possible answers..."
              << std::endl;

    const auto start_time = std::chrono::high_resolution_clock::now();

    const encoded_word best_word =
        find_best_guess_encoded(initial_indices, answers, all_words, false, 0,
                                0, feedback_ptr, lookups);

    const auto end_time = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "\n--- Calculation Complete ---" << std::endl;
    std::cout << "Best starting word: " << decode_word(best_word) << std::endl;
    std::cout << "Calculation time: " << elapsed.count() << " seconds."
              << std::endl;

  } else if (mode == "--word") {
    const encoded_word encoded_answer = encode_word(word_to_solve);

    const bool answer_is_valid =
        lookups.answer_index.find(encoded_answer) != lookups.answer_index.end();

    if (!answer_is_valid) {
      std::cerr << "Error: '" << word_to_solve
                << "' is not in the official answer list." << std::endl;
      return 1;
    }

    SolutionTrace trace;
    run_non_interactive(encoded_answer, answers, all_words, hard_mode, verbose,
                        !dump_json, &trace, feedback_ptr, lookups,
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
    } else if (!verbose) {
      for (const auto &step : trace.steps) {
        std::cout << decode_word(step.guess) << ' ';
      }
      std::cout << "\n";
    }
  } else {
    std::cerr << "Invalid arguments." << std::endl;
    std::cerr << "Usage: " << argv[0] << " --word <target_word> [--hard-mode]"
              << std::endl;
    std::cerr << "   or: " << argv[0] << " --find-best-start" << std::endl;
    return 1;
  }

  return 0;
}
