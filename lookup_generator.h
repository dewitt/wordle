#pragma once

#include <string>
#include <vector>

#include "feedback_cache.h"
#include "solver_core.h"
#include "words_data.h"

bool generate_lookup_table(const std::string &path,
                           const std::vector<encoded_word> &words,
                           encoded_word start, uint32_t depth,
                           const FeedbackTable *feedback_table,
                           const LookupTables &lookups);
