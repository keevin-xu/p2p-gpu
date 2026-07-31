# fuzz/corpus/

Committed fuzzing seeds. **These are evidence, not scratch files.**

## Rules

**Every crash found by a fuzzer gets its minimized input committed here** as a regression seed, in the same commit as the fix. That way the bug can never come back silently, and the corpus becomes a record of what was actually found rather than what was guessed at.

Organize by target:

```
corpus/
├── protocol/   frame parser seeds (fuzz_protocol)
└── asset/      chunk reassembly seeds (fuzz_asset, from Phase 4)
```

## Why this directory matters more than it looks

The most likely question a strong reader asks about a C++ system whose stated threat model is hostile workers is: *"what is your parser's memory-safety story?"*

"I used FlatBuffers" is a weak answer. "Here is the corpus, here are the exec counts, here is the ASan CI run, here is the crash I found in Phase 1 and the seed that pins it" is a strong one. The gap between those two answers is this directory being non-empty.

