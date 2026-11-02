# Code Style and Readability Requirements

## 1. General Principles

- Code readability and maintainability are mandatory.
- Prefer clear, explicit code over compact code.
- Do not sacrifice readability to reduce line count or token usage.
- Follow existing project conventions unless they conflict with these requirements.
- Keep changes focused. Do not reformat unrelated files.

## 2. Formatting

- Use 4 spaces for indentation. Never use tabs.
- Prefer a maximum line length of 100 characters.
- Never compress multiple statements onto one line.
- Use braces for all if/else, for, while and do-while bodies.
- Put control-flow bodies on separate lines.
- Do not write single-line function definitions.
- Do not write single-line nonempty lambda bodies.
- Break complex expressions and long function calls across lines.
- Separate logically distinct operations with blank lines.
- Preserve consistent indentation and brace placement.

## 3. C++ Requirements

- Target C++17 unless the project specifies otherwise.
- Prefer RAII and explicit resource ownership.
- Use meaningful names for variables, types and functions.
- Keep functions focused on one responsibility.
- Avoid unnecessary nesting and deeply chained expressions.
- Handle errors explicitly.
- Never hide complex logic inside ternary expressions.
- Avoid macros when ordinary C++ constructs suffice.
- Keep synchronization and object lifetime semantics clear.
- Do not introduce unnecessary abstractions.

## 4. Comments

- Explain intent, invariants and non-obvious design decisions.
- Document thread-safety assumptions and ownership boundaries.
- Do not write comments that merely repeat the code.
- Keep comments concise and technically accurate.

## 5. Verification

Before completing a coding task:

1. Review all modified code for readability.
2. Check indentation, braces and line lengths.
3. Expand compact control flow and one-line functions.
4. Run the project's configured formatter when available.
5. Run relevant compilation and tests when feasible.
6. Inspect the final diff for unintended changes.
7. Report any checks that could not be performed.

Code that works but violates these formatting rules is not considered complete.