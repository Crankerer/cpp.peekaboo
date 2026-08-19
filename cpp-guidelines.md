### Write modern, readable and maintainable C++

Generate modern, idiomatic C++ only. Use **C++20 or newer** where possible and make sensible use of modern language features and the standard library.

The code should not merely work, it should be **simple, short, clear, readable and maintainable in the long run**. Prefer the simplest solution that solves the problem cleanly and robustly.

### Core principles

- Prefer **C++20 or newer**.
- Write **short and concise code**, without hurting readability.
- Avoid unnecessary abstractions, wrappers, classes, helper functions and design patterns.
- **No over-engineering**: not every detail needs to be abstracted.
- Prefer simple, direct solutions over unnecessarily complex architectures.
- Code should be understandable at first glance.
- Use meaningful and short names for variables, functions and types.
- Avoid unnecessary comments. Comments should explain **why** something is done, not the obvious **what**.
- Avoid unnecessary nesting and long functions.
- Prefer early returns and guard clauses when they simplify control flow.
- Keep functions small and focused.
- Avoid code duplication, but do not create unnecessary abstractions for it.
- Prefer **composition over unnecessary inheritance**.

### Few files and a clear structure

Keep the number of files **as small as reasonably possible**.

- Do not create a separate file for every class, struct or small function.
- Small, closely related components may live in the same header/source file.
- Only split code across multiple files when that genuinely improves clarity or maintainability.
- Avoid unnecessary directory and module structures.
- The project structure should be understandable at a glance.
- Prefer a **flat and clear structure** over a deeply nested architecture.
- Avoid boilerplate and files that contain only a few lines, unless they serve a clear purpose.

### Modern C++ instead of C style

Avoid old C-style patterns whenever a modern C++ alternative exists:

- No manual `new`/`delete` calls -> use RAII and smart pointers.
- No C-style arrays -> use `std::array`, `std::vector` or a fitting container.
- No C-style casts -> use the appropriate C++ casts or better types.
- `nullptr` instead of `NULL` or `0`.
- `enum class` instead of plain enums.
- `std::string` / `std::string_view` instead of C strings.
- Use modern STL containers and algorithms.
- Use `std::optional`, `std::variant` and `std::expected` when they simplify the design.
- Use `constexpr`, `consteval`, `const`, `noexcept` and `[[nodiscard]]` sensibly.
- Use concepts, ranges and structured bindings when they genuinely make the code clearer.
- Use `auto` when it improves readability.
- No unnecessary macros.
- No unnecessary use of the C standard library when a better C++ alternative exists.
- If C APIs are necessary, encapsulate them in one clearly delimited place.

### Readability over cleverness

Always prefer:

**clear code > clever code**
**simple code > abstract code**
**less code > more boilerplate**
**few files > unnecessary splitting**
**modern C++ features > old C patterns**

Avoid extremely compact or "clever" code in particular: short but hard to understand is a bad trade. Brevity must never come at the cost of readability.

If a solution can be written in 10 clear lines instead of 3 cryptic ones, the clearer solution wins.

### Memory and resources

- Apply **RAII** consistently.
- Prefer `std::unique_ptr` over `std::shared_ptr` unless shared ownership is genuinely needed.
- Avoid raw pointers unless they are explicitly needed as non-owning pointers.
- Avoid unnecessary copies.
- Use move semantics sensibly, but not compulsively.
- Resources such as files, locks, sockets or handles must be managed automatically and safely.

### Error handling

- Use modern error handling that fits the situation.
- Prefer `std::optional` for "value present or not".
- Prefer `std::expected` for expected errors, where available.
- Use exceptions where they make sense.
- Avoid C-style error codes and manual error handling when a better C++ solution is possible.

### Prefer the standard library

Before writing custom solutions, check whether the C++ standard library already offers a fitting one.

Prefer for example:

- `std::vector`
- `std::array`
- `std::string`
- `std::string_view`
- `std::span`
- `std::optional`
- `std::variant`
- `std::expected`
- `std::filesystem`
- `std::chrono`
- `std::ranges`
- STL algorithms
- smart pointers
- lambdas

### Architecture

The architecture should be **as simple as possible and as structured as necessary**.

Avoid:

- unnecessary design patterns
- unnecessary interfaces
- unnecessary abstraction layers
- excessive template complexity
- deep inheritance hierarchies
- unnecessary dependency injection
- classes without real value
- unnecessary wrappers around STL types
- huge framework-like structures for small problems

Introduce an abstraction only when it brings a **concrete benefit** for readability, reuse, testability or maintainability.

### Goal

The finished code should look as if it had been written by an experienced C++ developer:

**modern, compact, clear, type-safe, robust, maintainable and free of unnecessary ballast.**

When several solutions are possible, prefer the one that achieves a clean and understandable result with **less code, fewer files and fewer abstractions** - as long as safety, maintainability and readability are preserved.
