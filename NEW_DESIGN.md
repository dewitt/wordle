The updated design is going to use a complete (but perhaps
non-optimal) pregenerated lookup tree for the guesses.

The structure of the tree is similar:

Node:
  - Guess -- index into a all words list
  - Feedback[256] - the feedback for that guess

The tree starts with a fixed root guess, known ahead of time. In this
example we are choosing "roate", but may choose another root guess
later, so it should be configurable.

The tree is built by an greedy depth first optimizing minimizing
search for every given word in the word list, with several novel
characteristics:

The evaluation criteria for the score of each node is the number of
candidate words remaining below that node in the tree. (Not just the
guess itself, but the intersection of all guesses above in the tree.)

Ties -- where two guesses have the same number of candidate words
remaining -- are broken by choosing the word with greater letter
frequency. The letter frequency table is constructed from the word
list.

After constructing the complete tree, there may be branches of depth
greater than 6 (greater than the maximum number of wordle guesses).

In those situations, we then begin a backtracking process, looking for
the deepest node in the graph that can be updated to reduce the depth
of all child subranches to conform to the max depth.

This may end up being an exhaustive search at each node, and we will
need to recalculate the entire subtree for each new guess, so we will
need to make extensive use of memoization and caching to efficiently
not have to recompute entire branches every time.

Additionally, because the tree data structure now needs to be mutable, we should consider keeping the entire thing in memory before serializing it.

Alternatively, we could use an engine such as sqlite to both manage and serialzing the graph.
