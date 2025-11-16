#include "feedback_cache.h"

#include <fstream>
#include <iostream>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "solver_core.h"
#include "words_data.h"

FeedbackTable::FeedbackTable() = default;

FeedbackTable::FeedbackTable(FeedbackTable &&other) noexcept {
  *this = std::move(other);
}

FeedbackTable &FeedbackTable::operator=(FeedbackTable &&other) noexcept {
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

FeedbackTable::~FeedbackTable() { release(); }

bool FeedbackTable::loaded() const { return mapped_data || !owned_data.empty(); }

const uint8_t *FeedbackTable::data() const {
  return mapped_data ? mapped_data : owned_data.data();
}

const uint8_t *FeedbackTable::row(size_t guess_idx) const {
  return data() + guess_idx * answer_count;
}

void FeedbackTable::release() {
#if defined(__unix__) || defined(__APPLE__)
  if (mapped_data) {
    munmap(const_cast<uint8_t *>(mapped_data), mapping_length);
  }
#endif
  mapped_data = nullptr;
  mapping_length = 0;
  guess_count = 0;
  answer_count = 0;
}

FeedbackTable load_feedback_table(const std::string &path,
                                  size_t word_count) {
  FeedbackTable table;
#if defined(__unix__) || defined(__APPLE__)
  const size_t expected_size = word_count * word_count;
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    struct stat st {};
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
