#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "feedback_cache.h"
#include "lookup_generator.h"
#include "solver_core.h"
#include "solver_runtime.h"
#include "words_data.h"

void print_usage(const char *prog_name) {
  std::cout
      << "Usage:\n"
      << "  " << prog_name << " solve <word> [--debug]\n"
      << "  " << prog_name << " start [--debug]\n"
      << "  " << prog_name
      << " generate [--lookup-depth N] [--lookup-output FILE]\n"
         "         [--lookup-start WORD] [--feedback-table]\n"
      << "  " << prog_name << " help\n\n"
      << "Flags:\n"
      << "  --debug           Verbose turn-by-turn output plus lookup "
         "diagnostics.\n"
      << "  --dump-json       Emit a JSON trace for solve mode instead of "
         "text.\n"
      << "  --lookup-depth N  Depth for lookup generation (default: 6).\n"
      << "  --lookup-output FILE  Output path for lookup table (default: "
         "lookup_<word>.bin).\n"
      << "  --lookup-start WORD   Start word when generating lookups "
         "(default: roate).\n"
      << "  --feedback-table  Rebuild feedback_table.bin before running.\n"
      << "  --help            Show this summary.\n";
}

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
  if (disable_lookup) {
    std::cerr << "--disable-lookup is not supported when using the "
                 "precomputed solver.\n";
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
  } else if (!positional.empty()) {
    if (generate_mode && lookup_output.empty() && positional.size() == 1) {
      lookup_output = positional.front();
    } else {
      std::cerr << "Unexpected positional arguments.\n";
      return 1;
    }
  }

  const auto &words = load_words();
  const LookupTables &lookups = load_lookup_tables();
  if (words.empty()) {
    std::cerr << "Embedded word list is empty. Exiting.\n";
    return 1;
  }

  if (rebuild_feedback_table) {
    if (!build_feedback_table_file(std::string(kFeedbackTablePath), words)) {
      return 1;
    }
  }

  FeedbackTable feedback_table =
      load_feedback_table(std::string(kFeedbackTablePath), words.size());
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
  if (solve_mode && !lookup_ptr) {
    std::cerr << "Lookup file 'lookup_roate.bin' not found. Run `"
              << "./build/solver generate --lookup-start roate --lookup-depth "
                 "6 --lookup-output lookup_roate.bin` first.\n";
    return 1;
  }

  if (start_mode) {
    std::vector<size_t> indices(words.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::cout << "Calculating the best starting word across " << words.size()
              << " valid words..." << std::endl;

    const auto start_time = std::chrono::high_resolution_clock::now();
    const encoded_word best_word =
        find_best_guess_encoded(indices, words, false, 0, 0, feedback_ptr,
                                lookups, load_word_weights());
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

  SolutionTrace trace;
  run_non_interactive(encoded_answer, words, hard_mode, debug_flag, !dump_json,
                      &trace, debug_flag, feedback_ptr, lookups, lookup_ptr);

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
  } else if (!debug_flag) {
    for (const auto &step : trace.steps) {
      std::cout << decode_word(step.guess) << ' ';
    }
    std::cout << "\n";
  }

  return 0;
}
