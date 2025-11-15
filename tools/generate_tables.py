#!/usr/bin/env python3
"""Generate precomputed word lists and opening table headers."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from typing import Iterable

DEFAULT_ANSWERS = Path("official_answers.txt")
DEFAULT_GUESSES = Path("official_guesses.txt")
WORD_LISTS_HEADER = Path("word_lists.h")
OPENING_TABLE_HEADER = Path("opening_table.h")


def encode(word: str) -> int:
    value = 0
    for char in word.strip():
        value = (value << 5) | (ord(char) - 96)
    return value


def decode(value: int) -> str:
    letters = []
    for i in range(5):
        code = (value >> (5 * (4 - i))) & 0x1F
        letters.append(chr(code + 96))
    return ''.join(letters)


def load_words(path: Path) -> list[str]:
    return [line.strip() for line in path.read_text().splitlines() if len(line.strip()) == 5]


def calculate_feedback(guess: int, answer: int) -> int:
    guess_codes = [0] * 5
    answer_codes = [0] * 5
    counts = [0] * 27

    for i in range(5):
        guess_codes[i] = (guess >> (5 * (4 - i))) & 0x1F
        answer_codes[i] = (answer >> (5 * (4 - i))) & 0x1F
        counts[answer_codes[i]] += 1

    feedback = [0] * 5
    for i in range(5):
        if guess_codes[i] == answer_codes[i]:
            feedback[i] = 2
            counts[guess_codes[i]] -= 1

    for i in range(5):
        if feedback[i] == 0 and counts[guess_codes[i]] > 0:
            feedback[i] = 1
            counts[guess_codes[i]] -= 1

    value = 0
    for digit in feedback:
        value = value * 3 + digit
    return value


def build_opening_table(answers: Iterable[int], solver: Path) -> list[int]:
    roate = encode("roate")
    table = [0] * 243
    seen: dict[int, str] = {}

    for value in answers:
        target = decode(value)
        result = subprocess.run([str(solver), "--word", target], capture_output=True, text=True, check=True)
        guess_lines = [line for line in result.stdout.splitlines() if line.startswith("Guess:")]
        if len(guess_lines) < 2:
            raise RuntimeError(f"Unexpected solver output for {target}:\n{result.stdout}")
        second_guess = guess_lines[1].split()[1].strip(',')
        feedback = calculate_feedback(roate, value)
        if feedback in seen and seen[feedback] != second_guess:
            raise RuntimeError(f"Feedback {feedback} mapped to both {seen[feedback]} and {second_guess}")
        seen[feedback] = second_guess
        table[feedback] = encode(second_guess)
    return table


def write_word_lists_header(answers: list[int], guesses: list[int], path: Path) -> None:
    with path.open("w") as out:
        out.write("#pragma once\n#include <cstddef>\n#include <cstdint>\n\n")
        out.write(f"constexpr std::size_t kAnswersCount = {len(answers)};\n")
        out.write(f"constexpr std::size_t kGuessesCount = {len(guesses)};\n")
        out.write("constexpr uint64_t kEncodedAnswers[kAnswersCount] = {\n")
        for i, val in enumerate(answers):
            out.write(f"    0x{val:016X}ULL{',' if i + 1 < len(answers) else ''}\n")
        out.write("};\n\nconstexpr uint64_t kEncodedGuesses[kGuessesCount] = {\n")
        for i, val in enumerate(guesses):
            out.write(f"    0x{val:016X}ULL{',' if i + 1 < len(guesses) else ''}\n")
        out.write("};\n")


def write_opening_table_header(table: list[int], path: Path) -> None:
    with path.open("w") as out:
        out.write("#pragma once\n#include <cstdint>\n\nconstexpr uint64_t kSecondGuessTable[243] = {\n")
        for i, val in enumerate(table):
            out.write(f"    0x{val:016X}ULL{',' if i + 1 < len(table) else ''}\n")
        out.write("};\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--answers", type=Path, default=DEFAULT_ANSWERS)
    parser.add_argument("--guesses", type=Path, default=DEFAULT_GUESSES)
    parser.add_argument("--solver", type=Path, default=Path("build/solver_cpp"))
    parser.add_argument("--word-lists", type=Path, default=WORD_LISTS_HEADER)
    parser.add_argument("--opening-table", type=Path, default=OPENING_TABLE_HEADER)
    args = parser.parse_args()

    answers = [encode(word) for word in load_words(args.answers)]
    guesses = [encode(word) for word in load_words(args.guesses)]

    write_word_lists_header(answers, guesses, args.word_lists)
    table = build_opening_table(answers, args.solver)
    write_opening_table_header(table, args.opening_table)


def main_entry() -> None:
    main()


if __name__ == "__main__":
    main()
