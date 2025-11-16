#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "feedback_cache.h"
#include "solver_core.h"
#include "words_data.h"

struct SolutionStep {
  encoded_word guess = 0;
  feedback_int feedback = 0;
};

struct SolutionTrace {
  std::vector<SolutionStep> steps;
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

class PrecomputedLookup {
public:
  bool load(const std::string &path, encoded_word expected_start);
  const uint8_t *root() const { return root_ptr_; }
  uint32_t depth() const { return depth_; }

  const uint8_t *find_child(const uint8_t *node, uint16_t feedback,
                            encoded_word &guess_out) const;

private:
  std::vector<uint8_t> buffer_;
  const uint8_t *root_ptr_ = nullptr;
  uint32_t depth_ = 0;
  encoded_word start_word_ = 0;
};

void run_non_interactive(encoded_word answer,
                         const std::vector<encoded_word> &words,
                         bool verbose, bool print_output, SolutionTrace *trace,
                         bool debug_lookup, const FeedbackTable *feedback_table,
                         const LookupTables &lookups,
                         const PrecomputedLookup *tree);
