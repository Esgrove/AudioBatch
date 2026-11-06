---
description: Declaration order and file organization of the C++ sources
globs: cpp/src/**,cpp/tests/**,test-plugin/src/**,image-control-plugin/src/**,image-control-plugin/tests/**
paths:
  - "cpp/src/**"
  - "cpp/tests/**"
  - "test-plugin/src/**"
  - "image-control-plugin/src/**"
  - "image-control-plugin/tests/**"
alwaysApply: false
---

# Code organization

These rules apply to every C++ source and header under `cpp/src/`, `cpp/tests/`, and `test-plugin/src/`,
and under `image-control-plugin/src/` and `image-control-plugin/tests/` for the ImageControl plugin.
Paths below are relative to `cpp/`, and `AGENTS.md` means `cpp/AGENTS.md`.

## Declaration order

Follow the [C++ Core Guidelines NL.16](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#Rl-order)
foundation: relevant types and aliases, special members, functions, then data.
Use `public`, then `protected`, then `private`, without scattering repeated access sections through the declaration.
Keep private implementation declarations in the private section.
At namespace scope, put dependent declarations after the types they need.
Related enums and small records normally precede their consuming class, but do not mechanically sort unrelated types.

The following are AudioBatch readability conventions, not requirements of the ISO C++ standard:

- Put constructors, the destructor, and copy/move operations first, followed by static methods.
  Separate the regular constructor group, destructor, and copy/move group with blank lines.
  Keep declarations within the constructor and copy/move groups contiguous.
  Static methods also lead the method declarations in protected and private sections.
- Keep ordinary methods separate from overrides, grouping overrides by the interface they implement when useful.
  An overriding destructor remains with the special members.
- Within each coherent method block, group by return type and alphabetize names within each group.
  Keep overloads together.
  Prefer complex types before basic scalar types: project/library objects, standard-library objects,
  then basic types such as `int`, `bool`, `float`, and `double`, with `void` last.
  Classify scalar aliases such as `juce::int64` and `std::int64_t` with basic types, not complex objects.
  Preserve related interface operations when a mechanical return-type split would reduce readability.
- Put methods before data within each access section.
  Put static data first within the data block, not ahead of methods.
- Group fields by type and alphabetize names within groups where practical.
  Prefer project/library objects, then standard-library objects, then basic scalar fields.
  Standard-library types go last among complex types, not after basic scalar fields.
  Correctness, lifetime dependencies, and observable contracts take priority over sorting.
- Split sections with more than six methods or fields into coherent groups.
  Smaller sections may remain compact.
  Put blank lines between groups, not between function declarations within a group.
  Preserve documentation and normal spacing between inline function definitions.

Follow [C.47](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#Rc-order)
by matching constructor initializer lists to declaration order.
When reordering fields, review initialization and reverse destruction dependencies,
positional and designated aggregate initializers, structured bindings, defaulted comparisons, and layout contracts.
Update callers where straightforward, but do not add manual teardown or worker lifecycle machinery solely for sorting.
Keep dependency-sensitive order and explain non-obvious requirements in a short comment.

## Types and namespaces

- Declare structs, classes and enums at namespace level, before the class that uses them.
  Do not define structs, classes or enums inside a class.
- A type that only one `.cpp` file uses goes in that file's anonymous namespace.
  This includes types that would otherwise be local to a function.
- File-local helper functions and variables also go in the anonymous namespace instead of being declared `static`.
  `misc-use-anonymous-namespace` in `.clang-tidy` enforces this.
- Give a `.cpp` file a single anonymous namespace, right after the project namespace opens and its using-declarations.
  Inside it, declare the types and constants first, then the helper functions.
  Wrap platform-specific helpers in `#if` blocks inside it instead of opening another anonymous namespace,
  and do not nest it in a named namespace of the file.
- Make enums `enum class`.
  Convert the integer ids JUCE passes for menu items, commands, and table columns back with `utils::enumFromId`,
  and switch over every enumerator without a `default` label.
- Put all project code in `namespace audiobatch`, so classes and their member definitions share it.
  The ImageControl plugin is not part of AudioBatch, so its code uses `namespace esgrove::image_control` instead.
  Keep `main`, the JUCE application macro, and forward declarations of third-party types outside it.
- Follow [C.4](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#Rc-member)
  and [C.5](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#Rc-helper):
  prefer nonmember helpers when direct access to a class representation is unnecessary,
  and put them in the namespace of the type they support.
  Related free helpers may share the type's header.
  Move shared helpers into a focused topic file when that improves cohesion or removes unnecessary dependencies.
  Do not turn a free helper into a static member merely to remove free functions from a class header.
- Do not write a class or struct whose members are all static.
  Use free functions in a namespace, as with `utils::` and `cli::`, and give them specific names.
  Keep classes for types that own state or enforce an invariant.
  Document each function on its definition and group related functions into one header per topic.
- Do not use `using namespace`, in headers or sources, at file or function scope.
  Wrap the definitions in the matching `namespace x { ... }` block or qualify the names.
- Do not use namespace aliases. Qualify references with the original namespace path.

## File size and structure

- Put each file in the `src/` subdirectory of its domain, as listed in the Source Layout section of `AGENTS.md`.
  Keep a header next to its source, and add a new directory only for a new domain with several files.
- Include project headers by their path relative to `src/`, such as `#include "analysis/AudioAnalysis.h"`,
  also from within the same directory, and never with `../`.
- Prefer a dedicated file for a substantial reusable class when that improves cohesion and dependencies.
  Small related helper types, records, and enums may remain together.
  Do not mechanically require one type per file or split implementation-local fixture helpers into public headers.
- Keep headers self-contained and include the declarations used directly rather than relying on transitive includes.
  Avoid cyclic dependencies.
- Split a file that reaches about 1000 lines into focused parts by topic.
  Each part should be a few hundred lines,
  with a shared internal header for constants and helpers used by several parts.
- Avoid very small files.
  Fold a tiny header or source into the file of the closest topic,
  or merge small related files into one.

## Documentation placement

- When a `.cpp` file exists for a header, document each function on its definition in the `.cpp`.
  Do not repeat the docstring on the declaration in the header.
- The header keeps the file-level comment and docs for types, enums, aliases, constants and data members.
- Code that has no `.cpp`, such as header-only files, inline functions, and templates,
  is documented where it is defined.
- Moved comments follow the semantic line break rules in `AGENTS.md`.

## Compile-time evaluation

- Use `constexpr` for every variable and function that can be evaluated at compile time.
- Use `consteval` for functions that must never run at runtime.
- Put file-local constants in an anonymous namespace as `constexpr` without a redundant `static`.
- Prefer named constants over unexplained numeric literals when values represent domain concepts, protocol fields,
  policy limits, or reused values. Keep self-evident arithmetic and conventional values inline.
- Function-local `static const` is fine for values that need runtime initialization, such as fonts.

## Initialization

- Prefer direct brace initialization for data members and namespace-scope constants,
  such as `int blockSize {512};` and `constexpr int fileSize {0};`,
  instead of copy initialization with `=`.
  Apply this to classes and structs, including those defined in source files.
- Do not extend this preference to local variables, loop initializers, or constructor initializer lists.
- Only add an empty `{}` initializer when value initialization is needed.
  Do not add redundant empty initializers to default-constructed class objects or members initialized by a constructor.
- Keep another initialization form when braces would change type deduction or constructor overload selection.
  Examples include container size constructors and copies of `juce::Array`.
- This preference applies to declarations, not later assignments or default function arguments.

## Includes

`clang-format` sorts and groups includes, so do not order them by hand.
Run `clang-format -i --style=file` on every changed file.

## Sanitizers

Use `./sanitize.sh` (the `macos-asan` preset) to run the tests under AddressSanitizer and UndefinedBehaviorSanitizer
when changing ownership, threading, or buffer handling code.
Use `./sanitize.sh --thread` (the `macos-tsan` preset) to run them under ThreadSanitizer when changing threading code.
On Windows, `./sanitize.sh` uses `windows-asan` with MSVC AddressSanitizer only.
ThreadSanitizer is not supported there.
