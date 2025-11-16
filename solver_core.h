#pragma once

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "solver_types.h"

struct FeedbackTable;

struct LookupTables {
  std::unordered_map<encoded_word, size_t> word_index;
};

const LookupTables &load_lookup_tables();
LookupTables build_lookup_tables_from_words(const std::vector<encoded_word> &words);

feedback_int calculate_feedback_encoded(encoded_word guess_encoded,
                                        encoded_word answer_encoded);

std::vector<size_t> filter_candidate_indices(
    const std::vector<size_t> &indices, encoded_word guess,
    feedback_int feedback, const FeedbackTable *feedback_table,
    const LookupTables &lookups, const std::vector<encoded_word> &words);

encoded_word find_best_guess_encoded(
    const std::vector<size_t> &possible_indices,
    const std::vector<encoded_word> &words,
    const FeedbackTable *feedback_table, const LookupTables &lookups,
    const std::vector<uint32_t> &weights,
    const std::unordered_set<encoded_word> *banned_guesses = nullptr);
