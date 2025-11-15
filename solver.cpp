// (Existing functions: encode_word, decode_word, get_char_code_at, calculate_feedback_encoded, filter_word_list_encoded)

/**
 * solver.cpp - A highly optimized, multithreaded Wordle solver in C++.
 *
 * This program uses an entropy-reduction algorithm to find the best possible
 * Wordle guess. It has been optimized for performance through several techniques:
 *
 * 1.  Integer Word Representation: 5-letter words are encoded into 64-bit
 *     unsigned integers (uint64_t), where each character is represented by 5
 *     bits. This makes comparisons and storage extremely fast and cache-friendly.
 *
 * 2.  Bitwise Feedback Calculation: The core feedback logic operates directly on
 *     these encoded integers using bitwise operations, avoiding costly string
 *     manipulations in the performance-critical sections.
 *
 * 3.  Multithreading: The search for the best guess is parallelized across all
 *     available CPU cores using std::async, making the calculation significantly
 *     faster.
 *
 * 4.  Modern C++ Idioms: The code uses modern C++17 features like std::string_view,
 *     std::async, and const correctness for safety, clarity, and performance.
 *
 * It can be used to solve a specific Wordle puzzle or to run a one-off analysis
 * to find the single best starting word from scratch.
 */
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <string_view>
#include <numeric>
#include <algorithm>
#include <iterator>
#include <limits>
#include <thread>
#include <future>
#include <chrono>
#include <array>
#include <cstdint>
#include <fstream>

// --- Type Definitions for Optimization ---
using encoded_word = uint64_t;
using feedback_int = int;

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

std::vector<encoded_word> load_word_list_encoded(const std::string& path) {
    std::ifstream file(path);
    std::vector<encoded_word> words;
    std::string line;
    if (!file.is_open()) {
        std::cerr << "Error: Word list file not found at '" << path << "'" << std::endl;
        return words;
    }
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.length() == 5) {
            words.push_back(encode_word(line));
        }
    }
    return words;
}

constexpr encoded_word kInitialGuess = encode_word("roate");

// --- Core Logic (Operating on Encoded Data) ---

// Helper to get the encoded character (1-26) at a specific position (0-4).
inline uint8_t get_char_code_at(encoded_word word, int pos) {
    return (word >> (5 * (4 - pos))) & 0x1F;
}

// Calculates feedback for two encoded words and returns an encoded feedback integer.
// This version operates purely on integers and bitwise operations.
feedback_int calculate_feedback_encoded(encoded_word guess_encoded, encoded_word answer_encoded) {
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

// Filters a list of encoded words based on a guess and its feedback.
std::vector<encoded_word> filter_word_list_encoded(
    const std::vector<encoded_word>& words, 
    encoded_word guess, 
    feedback_int feedback) 
{
    std::vector<encoded_word> new_list;
    new_list.reserve(words.size() / 5); // Pre-allocate memory
    for (const auto& word : words) {
        if (calculate_feedback_encoded(guess, word) == feedback) {
            new_list.push_back(word);
        }
    }
    return new_list;
}

// --- Hard Mode Logic ---

// Checks if a potential guess is valid under hard mode rules.
bool is_valid_hard_mode_guess(encoded_word potential_guess, encoded_word previous_guess, feedback_int previous_feedback) {
    uint8_t prev_guess_codes[5];
    uint8_t potential_guess_codes[5];
    uint8_t feedback_codes[5];
    
    int temp_feedback = previous_feedback;
    for(int i = 4; i >= 0; --i) {
        feedback_codes[i] = temp_feedback % 3;
        temp_feedback /= 3;
    }

    uint8_t required_yellows[27] = {0};

    for (int i = 0; i < 5; ++i) {
        prev_guess_codes[i] = get_char_code_at(previous_guess, i);
        potential_guess_codes[i] = get_char_code_at(potential_guess, i);

        // Rule 1: Green letters must be in the same spot.
        if (feedback_codes[i] == 2 && potential_guess_codes[i] != prev_guess_codes[i]) {
            return false;
        }
        // Collect required yellow letters.
        if (feedback_codes[i] == 1) {
            required_yellows[prev_guess_codes[i]]++;
        }
    }

    // Rule 2: Yellow letters must be present in the new guess.
    uint8_t potential_guess_counts[27] = {0};
    for(int i = 0; i < 5; ++i) {
        potential_guess_counts[potential_guess_codes[i]]++;
    }

    for(int i = 1; i <= 26; ++i) {
        if (potential_guess_counts[i] < required_yellows[i]) {
            return false;
        }
    }

    return true;
}


// The worker task for std::async to find the best guess in a subset of words.
std::pair<encoded_word, double> find_best_guess_worker(
    const std::vector<encoded_word>& possible_words,
    const std::vector<encoded_word>& guess_subset) 
{
    encoded_word local_best_guess = 0;
    double local_min_score = std::numeric_limits<double>::max();

    for (const auto& guess : guess_subset) {
        std::array<int, 243> feedback_groups{};
        double current_score = 0.0;
        bool pruned = false;
        for (const auto& answer : possible_words) {
            const feedback_int feedback = calculate_feedback_encoded(guess, answer);
            const int count_before = feedback_groups[feedback];
            current_score += static_cast<double>(2 * count_before + 1);
            feedback_groups[feedback] = count_before + 1;
            if (current_score >= local_min_score) {
                pruned = true;
                break;
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
    const std::vector<encoded_word>& possible_words, 
    const std::vector<encoded_word>& all_words,
    bool hard_mode,
    encoded_word previous_guess,
    feedback_int previous_feedback) 
{
    if (possible_words.empty()) {
        return 0;
    }

    // In hard mode, we must first filter the list of all possible guesses.
    const std::vector<encoded_word>* guesses_to_check = &all_words;
    std::vector<encoded_word> valid_hard_mode_guesses;
    if (hard_mode && previous_guess != 0) {
        valid_hard_mode_guesses.reserve(all_words.size() / 10);
        for (const auto& guess : all_words) {
            if (is_valid_hard_mode_guess(guess, previous_guess, previous_feedback)) {
                valid_hard_mode_guesses.push_back(guess);
            }
        }
        guesses_to_check = &valid_hard_mode_guesses;
    }

    constexpr size_t kAnswerOnlyThreshold = 1024;
    if (possible_words.size() <= kAnswerOnlyThreshold) {
        guesses_to_check = &possible_words;
    }

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::vector<std::vector<encoded_word>> word_chunks(num_threads);
    for (size_t i = 0; i < guesses_to_check->size(); ++i) {
        word_chunks[i % num_threads].push_back((*guesses_to_check)[i]);
    }

    std::vector<std::future<std::pair<encoded_word, double>>> futures;
    for (unsigned int i = 0; i < num_threads; ++i) {
        if (!word_chunks[i].empty()) {
            futures.push_back(std::async(std::launch::async, find_best_guess_worker, 
                                         std::cref(possible_words), 
                                         std::cref(word_chunks[i])));
        }
    }

    encoded_word best_guess = 0;
    double min_overall_score = std::numeric_limits<double>::max();

    for (auto& fut : futures) {
        auto result = fut.get();
        if (result.second < min_overall_score) {
            min_overall_score = result.second;
            best_guess = result.first;
        }
    }

    return best_guess;
}

// --- Main Application Logic ---

void run_non_interactive(
    encoded_word answer, 
    const std::vector<encoded_word>& answers, 
    const std::vector<encoded_word>& all_words,
    bool hard_mode) 
{
    auto possible_words = answers;
    int turn = 1;
    encoded_word guess = 0;
    feedback_int feedback_val = 0;

    std::array<std::vector<encoded_word>, 243> roate_partitions;
    std::array<encoded_word, 243> second_guess_cache{};
    if (!hard_mode) {
        for (const auto& candidate : answers) {
            const feedback_int fb = calculate_feedback_encoded(kInitialGuess, candidate);
            roate_partitions[fb].push_back(candidate);
        }
    }

    std::cout << "Solving for: " << decode_word(answer) << (hard_mode ? " (Hard Mode)" : "") << std::endl;
    std::cout << "------------------------------" << std::endl;

    while (turn <= 6 && guess != answer) {
        std::cout << "Turn " << turn << " (" << possible_words.size() << " possibilities remain)" << std::endl;

        if (turn == 1) {
            guess = kInitialGuess;
        } else {
            if (possible_words.size() == 1) {
                guess = possible_words[0];
            } else if (!hard_mode && turn == 2) {
                encoded_word& cached = second_guess_cache[feedback_val];
                if (cached == 0) {
                    const auto& subset = roate_partitions[feedback_val];
                    if (subset.empty()) {
                        cached = find_best_guess_encoded(possible_words, all_words, false, kInitialGuess, feedback_val);
                    } else if (subset.size() == 1) {
                        cached = subset[0];
                    } else {
                        cached = find_best_guess_encoded(subset, all_words, false, kInitialGuess, feedback_val);
                    }
                }
                guess = cached;
            } else {
                guess = find_best_guess_encoded(possible_words, all_words, hard_mode, guess, feedback_val);
            }
        }

        if (guess == 0) {
            std::cout << "Solver failed to find a valid guess." << std::endl;
            break;
        }

        feedback_val = calculate_feedback_encoded(guess, answer);
        
        // Manually decode feedback for printing
        std::string feedback_str;
        feedback_str.reserve(5);
        int temp_feedback = feedback_val;
        for(int i = 0; i < 5; ++i) {
            const int remainder = temp_feedback % 3;
            if (remainder == 2) feedback_str += 'g';
            else if (remainder == 1) feedback_str += 'y';
            else feedback_str += '_';
            temp_feedback /= 3;
        }
        std::reverse(feedback_str.begin(), feedback_str.end());

        std::cout << "Guess: " << decode_word(guess) << ", Feedback: " << feedback_str << std::endl;

        if (feedback_str == "ggggg") {
            std::cout << "\nSolved in " << turn << " guesses!" << std::endl;
            return;
        }

        possible_words = filter_word_list_encoded(possible_words, guess, feedback_val);
        turn++;
    }

    if (guess != answer) {
        std::cout << "\nSolver failed to find the word. Last guess was '" << decode_word(guess) << "'." << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Fast I/O
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " --word <target_word> [--hard-mode]" << std::endl;
        std::cerr << "   or: " << argv[0] << " --find-best-start" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    std::string word_to_solve;
    bool hard_mode = false;

    if (mode == "--word") {
        if (argc < 3) {
            std::cerr << "Error: --word requires a target word." << std::endl;
            return 1;
        }
        word_to_solve = argv[2];
        if (argc == 4 && std::string(argv[3]) == "--hard-mode") {
            hard_mode = true;
        }
    }

    const auto answers = load_word_list_encoded("official_answers.txt");
    const auto guesses = load_word_list_encoded("official_guesses.txt");

    if (answers.empty()) {
        std::cerr << "Could not load official answers list. Exiting." << std::endl;
        return 1;
    }

    std::vector<encoded_word> all_words = answers;
    all_words.insert(all_words.end(), guesses.begin(), guesses.end());

    if (mode == "--find-best-start") {
        std::cout << "Calculating the best starting word from " << all_words.size() 
                  << " guesses against " << answers.size() << " possible answers..." << std::endl;
        
        const auto start_time = std::chrono::high_resolution_clock::now();
        
        const encoded_word best_word = find_best_guess_encoded(answers, all_words, false, 0, 0);
        
        const auto end_time = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> elapsed = end_time - start_time;

        std::cout << "\n--- Calculation Complete ---" << std::endl;
        std::cout << "Best starting word: " << decode_word(best_word) << std::endl;
        std::cout << "Calculation time: " << elapsed.count() << " seconds." << std::endl;

    } else if (mode == "--word") {
        const encoded_word encoded_answer = encode_word(word_to_solve);
        
        const bool answer_is_valid = std::find(answers.begin(), answers.end(), encoded_answer) != answers.end();

        if (!answer_is_valid) {
            std::cerr << "Error: '" << word_to_solve << "' is not in the official answer list." << std::endl;
            return 1;
        }

        run_non_interactive(encoded_answer, answers, all_words, hard_mode);
    } else {
        std::cerr << "Invalid arguments." << std::endl;
        std::cerr << "Usage: " << argv[0] << " --word <target_word> [--hard-mode]" << std::endl;
        std::cerr << "   or: " << argv[0] << " --find-best-start" << std::endl;
        return 1;
    }

    return 0;
}
