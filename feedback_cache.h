#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "solver_types.h"

struct FeedbackTable {
  size_t guess_count = 0;
  size_t answer_count = 0;
  std::vector<uint8_t> owned_data;
  const uint8_t *mapped_data = nullptr;
  size_t mapping_length = 0;

  FeedbackTable();
  FeedbackTable(const FeedbackTable &) = delete;
  FeedbackTable &operator=(const FeedbackTable &) = delete;
  FeedbackTable(FeedbackTable &&other) noexcept;
  FeedbackTable &operator=(FeedbackTable &&other) noexcept;
  ~FeedbackTable();

  bool loaded() const;
  const uint8_t *data() const;
  const uint8_t *row(size_t guess_idx) const;

private:
  void release();
};

FeedbackTable load_feedback_table(const std::string &path,
                                  size_t word_count);
bool build_feedback_table_file(const std::string &path,
                               const std::vector<encoded_word> &words);
