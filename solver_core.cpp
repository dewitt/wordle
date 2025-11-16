#include "solver_core.h"

#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <numeric>
#include <thread>

#include "feedback_cache.h"
#include "words_data.h"

feedback_int calculate_feedback_encoded(encoded_word guess_encoded,
                                        encoded_word answer_encoded) {
  uint8_t guess_codes[5];
  uint8_t answer_codes[5];
  uint8_t answer_counts[27] = {0};

  for (int i = 0; i < 5; ++i) {
    guess_codes[i] = get_char_code_at(guess_encoded, i);
    answer_codes[i] = get_char_code_at(answer_encoded, i);
    answer_counts[answer_codes[i]]++;
  }

  uint8_t feedback_codes[5] = {0};

  for (int i = 0; i < 5; ++i) {
    if (guess_codes[i] == answer_codes[i]) {
      feedback_codes[i] = 2;
      answer_counts[guess_codes[i]]--;
    }
  }

  for (int i = 0; i < 5; ++i) {
    if (feedback_codes[i] == 0 && answer_counts[guess_codes[i]] > 0) {
      feedback_codes[i] = 1;
      answer_counts[guess_codes[i]]--;
    }
  }

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

    if (feedback_codes[i] == 2 &&
        potential_guess_codes[i] != prev_guess_codes[i]) {
      return false;
    }
    if (feedback_codes[i] == 1) {
      required_yellows[prev_guess_codes[i]]++;
    }
  }

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

std::pair<encoded_word, double>
find_best_guess_worker(const std::vector<size_t> &possible_indices,
                       const std::vector<encoded_word> &guess_subset,
                       const FeedbackTable *feedback_table,
                       const LookupTables &lookups,
                       const std::vector<encoded_word> &words,
                       const std::vector<uint32_t> &weights) {
  encoded_word local_best_guess = 0;
  double local_min_score = std::numeric_limits<double>::max();
  uint32_t local_best_weight = 0;

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
        const uint8_t fb = row[idx];
        const int count_before = feedback_groups[fb];
        current_score += static_cast<double>(2 * count_before + 1);
        feedback_groups[fb] = count_before + 1;
        if (current_score >= local_min_score) {
          pruned = true;
          break;
        }
      }
    } else {
      for (const auto idx : possible_indices) {
        const feedback_int fb =
            calculate_feedback_encoded(guess, words[idx]);
        const int count_before = feedback_groups[fb];
        current_score += static_cast<double>(2 * count_before + 1);
        feedback_groups[fb] = count_before + 1;
        if (current_score >= local_min_score) {
          pruned = true;
          break;
        }
      }
    }

    if (!pruned) {
      uint32_t guess_weight = 0;
      const auto weight_it = lookups.word_index.find(guess);
      if (weight_it != lookups.word_index.end()) {
        guess_weight = weights[weight_it->second];
      }
      if (current_score < local_min_score ||
          (current_score == local_min_score &&
           guess_weight > local_best_weight)) {
        local_min_score = current_score;
        local_best_guess = guess;
        local_best_weight = guess_weight;
      }
    }
  }
  return {local_best_guess, local_min_score};
}

encoded_word find_best_guess_encoded(
    const std::vector<size_t> &possible_indices,
    const std::vector<encoded_word> &words, bool hard_mode,
    encoded_word previous_guess, feedback_int previous_feedback,
    const FeedbackTable *feedback_table, const LookupTables &lookups,
    const std::vector<uint32_t> &weights) {
  if (possible_indices.empty()) {
    return 0;
  }

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
                                   std::cref(lookups), std::cref(words),
                                   std::cref(weights)));
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
