# Code organization

These rules apply to every C++ source and header under `src/` and `tests/`.

## Declaration order

Within a header or namespace, declare in this order:

1. Enums.
2. Structs.
3. Classes.

Within a struct or class:

1. `public` section, then `protected`, then `private`.
2. Member functions before data members in each section.
3. Group functions and variables by return type and topic.
   Keep related functions together, and keep functions that return the same kind of value next to each other.

## Types and namespaces

- Declare structs, classes and enums at namespace level, before the class that uses them.
  Do not define structs, classes or enums inside a class.
- A type that only one `.cpp` file uses goes in that file's anonymous namespace.
  This includes types that would otherwise be local to a function.
- Put all project code in `namespace audiobatch`, so classes and their member definitions share it.
  Keep `main`, the JUCE application macro, and forward declarations of third-party types outside it.
- Do not use `using namespace`, in headers or sources, at file or function scope.
  Wrap the definitions in the matching `namespace x { ... }` block or qualify the names.

## File size and structure

- Split a file that reaches about 1000 lines into focused parts by topic.
  Each part should be a few hundred lines, with a shared internal header for constants and helpers used by several parts.
- Avoid very small files.
  Fold a tiny header or source into the file of the closest topic,
  or merge small related files into one.

## Documentation placement

- When a `.cpp` file exists for a header, document each function on its definition in the `.cpp`.
  Do not repeat the docstring on the declaration in the header.
- The header keeps the file-level comment and docs for types, enums, aliases, constants and data members.
- Code that has no `.cpp`, such as header-only files, inline functions, and templates, is documented where it is defined.
- Moved comments follow the semantic line break rules in `AGENTS.md`.

## Compile-time evaluation

- Use `constexpr` for every variable and function that can be evaluated at compile time.
- Use `consteval` for functions that must never run at runtime.
- Put file-local constants in an anonymous namespace as `constexpr` without a redundant `static`.
- Function-local `static const` is fine for values that need runtime initialization, such as fonts.

## Includes

`clang-format` sorts and groups includes, so do not order them by hand.
Run `clang-format -i --style=file` on every changed file.

## Sanitizers

Use `./sanitize.sh` (the `macos-asan` preset) to run the tests under AddressSanitizer and UndefinedBehaviorSanitizer
when changing ownership, threading, or buffer handling code.
