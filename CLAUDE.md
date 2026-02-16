# ECS Project — Claude Code Guidelines

## Project

Archetype-based ECS library. Header-only C++17, no external dependencies.
See `SPEC.md` for design and API contracts. See `IMPLEMENTATION.md` for the
phased build plan and current progress.

## Build

```bash
cd build && cmake .. && make && ./ecs_test
```

Sanitizer build:
```bash
cmake -DECS_SANITIZE=ON .. && make && ./ecs_test
```

After every code change, **build and run tests before considering the task done.**
Zero warnings required (`-Wall -Wextra -Wpedantic`).

## Development Workflow

All feature work and architectural changes are planned via RFCs in
`docs/rfcs/`. See `docs/rfcs/README.md` for the pipeline and index.

1. **Plan:** Draft an RFC from `docs/rfcs/_TEMPLATE.md` into `00-proposals/`.
2. **Activate:** Move the RFC to `01-active/` when implementation begins.
3. **Implement:** Work incrementally. Each commit must compile clean, pass all
   tests, and maintain zero warnings.
4. **Test:** New functionality must include tests in `tests/main.cpp`.
5. **Document:** When a change affects behavior covered by `SPEC.md`, update
   the spec to match. The spec is the source of truth — keep it accurate.
6. **Complete:** Move the RFC to `02-implemented/` and update the index.

`IMPLEMENTATION.md` tracks phase-level progress (what's done vs. planned).
RFCs describe the *why* and *how* of each change in detail.

## Code Style

Formatting is enforced via `.clang-format`. Run `clang-format -i` on changed
files before committing.

Follow the conventions established in the existing code:

- **Naming:** `PascalCase` for types/structs/classes. `snake_case` for
  functions, methods, and variables. Trailing underscore for private members
  (`records_`, `free_list_`).
- **Headers:** `#pragma once`. Group includes: project headers first, then
  standard library, alphabetized within each group.
- **Namespace:** Everything lives in `namespace ecs {}`.
- **No RTTI, no exceptions** in core paths. Use `ECS_ASSERT` (once introduced)
  or `assert` for precondition checks.
- **Minimal comments.** Comment the *why*, not the *what*. No boilerplate
  docstrings on self-explanatory methods.
- **No unnecessary abstractions.** If something is used once, inline it.
  Don't add indirection for hypothetical future needs.

## Architecture Rules

- **Header-only.** No `.cpp` files in `include/`. The only `.cpp` is the test
  executable.
- **No external dependencies** in the core library. Test files may use external
  tools (sanitizers, etc.) but the library itself is stdlib-only.
- **Builtins are ordinary components.** Nothing in `builtin/` gets special
  treatment from the core. If a feature requires the core to know about a
  specific component type, the design is wrong.
- **World is the API boundary.** Users interact with `World`. Internal types
  (`Archetype`, `ComponentColumn`, `EntityRecord`) are implementation details.
  Don't expand their public surface without good reason.
- **Invariants are sacred.** The six invariants in SPEC §6 must hold after
  every public method returns. If a change would violate one, either fix the
  change or update the invariant (with justification in the spec).

## What Not To Do

- Don't add features without an RFC in `docs/rfcs/`.
- Don't refactor working code unless the current task requires it.
- Don't add dependencies.
- Don't break existing tests to make new ones pass.
- Don't commit with warnings.
