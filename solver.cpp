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
#include <fstream>
#include <unordered_map>
#include <string_view>
#include <numeric>
#include <algorithm>
#include <limits>
#include <thread>
#include <future>
#include <chrono>

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

// --- Core Logic (Operating on Encoded Data) ---

// Loads a word list from a text file and returns a vector of encoded words.
std::vector<encoded_word> load_word_list_encoded(const std::string& path) {
    std::ifstream file(path);
    std::vector<encoded_word> words;
    std::string line;
    if (!file.is_open()) {
        std::cerr << "Error: Word list file not found at '" << path << "'" << std::endl;
        return words;
    }
    while (std::getline(file, line)) {
        if (line.length() == 5) {
            words.push_back(encode_word(line));
        }
    }
    return words;
}

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

// The worker task for std::async to find the best guess in a subset of words.
std::pair<encoded_word, double> find_best_guess_worker(
    const std::vector<encoded_word> possible_words,
    const std::vector<encoded_word> guess_subset) 
{
    encoded_word local_best_guess = 0;
    double local_min_score = std::numeric_limits<double>::max();

    for (const auto& guess : guess_subset) {
        std::unordered_map<feedback_int, int> feedback_groups;
        for (const auto& answer : possible_words) {
            feedback_groups[calculate_feedback_encoded(guess, answer)]++;
        }

        double current_score = 0.0;
        for (const auto& pair : feedback_groups) {
            current_score += static_cast<double>(pair.second * pair.second);
        }

        if (current_score < local_min_score) {
            local_min_score = current_score;
            local_best_guess = guess;
        }
    }
    return {local_best_guess, local_min_score};
}

// Finds the best word to guess next using a pool of asynchronous tasks.
encoded_word find_best_guess_encoded(
    const std::vector<encoded_word>& possible_words, 
    const std::vector<encoded_word>& all_words) 
{
    if (possible_words.empty()) {
        return 0;
    }

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::vector<std::vector<encoded_word>> word_chunks(num_threads);
    for (size_t i = 0; i < all_words.size(); ++i) {
        word_chunks[i % num_threads].push_back(all_words[i]);
    }

    std::vector<std::future<std::pair<encoded_word, double>>> futures;
    for (unsigned int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, find_best_guess_worker, 
                                     std::cref(possible_words), 
                                     std::cref(word_chunks[i])));
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
    const std::vector<encoded_word>& all_words) 
{
    auto possible_words = answers;
    int turn = 1;
    encoded_word guess = 0;

    std::cout << "Solving for: " << decode_word(answer) << std::endl;
    std::cout << "------------------------------" << std::endl;

    while (turn <= 6 && guess != answer) {
        std::cout << "Turn " << turn << " (" << possible_words.size() << " possibilities remain)" << std::endl;

        if (turn == 1) {
            guess = encode_word("roate");
        } else {
            if (possible_words.size() == 1) {
                guess = possible_words[0];
            } else {
                guess = find_best_guess_encoded(possible_words, all_words);
            }
        }

        if (guess == 0) {
            std::cout << "Solver failed to find a guess." << std::endl;
            break;
        }

        const feedback_int feedback_val = calculate_feedback_encoded(guess, answer);
        
        // Manually decode feedback for printing
        std::string feedback_str;
        feedback_str.reserve(5);
        int temp_feedback = feedback_val;
        // There are 243 possible feedbacks (3^5)
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
        std::cerr << "Usage: " << argv[0] << " --word <target_word>" << std::endl;
        std::cerr << "   or: " << argv[0] << " --find-best-start" << std::endl;
        return 1;
    }

    const std::string mode = argv[1];

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
        
        const encoded_word best_word = find_best_guess_encoded(answers, all_words);
        
        const auto end_time = std::chrono::high_resolution_clock::now();
        const std::chrono::duration<double> elapsed = end_time - start_time;

        std::cout << "\n--- Calculation Complete ---" << std::endl;
        std::cout << "Best starting word: " << decode_word(best_word) << std::endl;
        std::cout << "Calculation time: " << elapsed.count() << " seconds." << std::endl;

    } else if (mode == "--word" && argc == 3) {
        const std::string word_to_solve = argv[2];
        const encoded_word encoded_answer = encode_word(word_to_solve);
        
        const bool answer_is_valid = std::find(answers.begin(), answers.end(), encoded_answer) != answers.end();

        if (!answer_is_valid) {
            std::cerr << "Error: '" << word_to_solve << "' is not in the official answer list." << std::endl;
            return 1;
        }

        run_non_interactive(encoded_answer, answers, all_words);
    } else {
        std::cerr << "Invalid arguments." << std::endl;
        std::cerr << "Usage: " << argv[0] << " --word <target_word>" << std::endl;
        std::cerr << "   or: " << argv[0] << " --find-best-start" << std::endl;
        return 1;
    }

    return 0;
}