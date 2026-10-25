# AK3Y Instrument Controller — Coding and Library Standards

**Document ID:** CSD  
**Revision:** 2.0 (documentation consolidation)  
**Status:** Proposed — review before adoption  
**Project:** AK3Y Instrument Controller

## 1. Purpose and scope

This document establishes maintainability and development conventions for the
controller's C++ firmware and local PlatformIO libraries. It supersedes the
separate *Library Development Standard* after approval. The existing code is
the implementation reference; these standards guide new work and maintenance,
not a mandate for unrelated refactoring.

Correctness comes first, readability second, and optimization where justified.
Maintain the firmware in a buildable state and make changes in small,
reviewable increments.

## 2. File organization and documentation

Each nonempty `.cpp` and `.h` file should begin with a concise descriptive
comment identifying the file, project, purpose, primary responsibilities,
and relevant dependencies or implementation constraints. Header comments
should explain the module's current role rather than recount release history.

Use `#pragma once` or a conventional include guard in headers. Keep includes
near the beginning of each file, followed by declarations or definitions.
Where practical, order includes as: corresponding header, standard library,
third-party libraries, then other project headers. Remove unused includes
when making an appropriate code change.

Keep meaningful comments about **why** code works a particular way, especially
IEEE-488 handshakes, hardware signal polarity, timing, error handling, and
embedded HTML/JavaScript escaping. Avoid repetitive comments that merely
restate the code. Document complex public interfaces and nonobvious
preconditions, side effects, and ownership.

**Per-library `README.md` files are not required.** Module documentation
belongs primarily in source headers and technical comments. Cross-module
architecture and interface specifications belong in `docs/`; the repository
root `README.md` describes the project and build procedure. Git preserves
historical changes; do not accumulate per-file milestone histories.

## 3. Library layout and manifests

Local libraries generally follow the existing PlatformIO layout:

```text
lib/LibraryName/
    LibraryName.h
    LibraryName.cpp       # when needed
    library.json          # when used by PlatformIO
```

Some libraries contain additional implementation files or headers; preserve
their existing structure when justified. Retain `library.json` manifests
that supply PlatformIO build metadata, dependencies, or other required
settings. Do not remove JSON configuration or editor settings solely to
simplify documentation.

Library responsibilities should be cohesive, with a small public interface
and explicit dependencies. Keep hardware access, protocol handling, and
application orchestration distinct where practical.

## 4. Naming and formatting

Preferred conventions for new code, consistent with the original standard:

- Classes and types: `PascalCase` (`Application`, `Console`).
- Methods and functions: `camelCase` (`initialize`, `printBanner`).
- Member variables: `m_` prefix (`m_console`).
- Public constants: `PascalCase`; private compile-time constants may use `kPrefix`.
- Prefer four-space indentation, braces on separate lines, and logical
  whitespace; aim for lines of approximately 100 characters when practical.

Follow surrounding code where necessary to avoid introducing unrelated
formatting changes. Do not reformat generated HTML/JavaScript strings as part
of a documentation cleanup.

## 5. Classes, dependencies, and ownership

- Give each class or library a clearly defined primary responsibility.
- Keep public interfaces minimal; hide implementation details.
- Prefer composition over unnecessary inheritance.
- Make object ownership and lifetime explicit.
- Prefer constructor injection for required service dependencies where
  appropriate; avoid hidden global state.
- Keep interfaces stable and avoid coupling unrelated subsystems.

Record significant ownership relationships and architectural constraints in
`SoftwareArchitecture.md` or the relevant source header, rather than in
separate library READMEs.

## 6. Functions and initialization

Functions should perform a focused task. Prefer const correctness and passing
large read-only objects by const reference where appropriate. Avoid making
functions artificially short when a coherent sequence is clearer intact.

Initialization should be explicit. Constructors should establish valid object
state without performing failure-prone hardware operations when practical.
Document initialization order and dependencies where they matter. Existing
`initialize()` conventions may be retained; do not assume every module has
an identical return type or lifecycle.

## 7. Error handling and diagnostics

Check and report meaningful failures. Avoid silently discarding errors.
Preserve timeout handling, diagnostic messages, and safeguards for hardware
and SD-card operations. A change to externally visible diagnostics or error
behavior is a functional change and should be tested as such.

## 8. Development and verification workflow

For each focused change:

1. Identify the requirement and affected modules.
2. Review dependencies, hardware implications, and existing behavior.
3. Implement the smallest practical change.
4. Compile in PlatformIO and address warnings relevant to the change.
5. Perform appropriate bench or browser tests when behavior changes.
6. Update source comments and architectural documentation as needed.
7. Inspect the Git diff for unintended changes.
8. Commit a coherent milestone and synchronize with GitHub.

For documentation-only changes, a diff review may be sufficient when no
firmware or build configuration changes; compiling remains a useful optional
check. For firmware changes, compilation alone does not establish hardware
correctness.

Significant engineering changes may be documented with an Engineering Change
Request (ECR) or equivalent review notes when useful. Git commits are the
primary revision record; separate change-history tables are not mandatory.

## 9. Acceptance checklist

Before committing a library or module change, verify:

- The module's responsibility and dependencies are clear.
- Public interfaces, ownership, and important constraints are documented.
- Source headers describe current behavior without obsolete release notes.
- Required PlatformIO manifests and configuration files are preserved.
- Functional changes have been compiled and tested as appropriate.
- Documentation-only changes have been checked for unintended code edits.
- The Git diff contains only the intended changes.

## 10. Related documentation

- `README.md` — project overview and build instructions.
- `docs/SoftwareArchitecture.md` — firmware components and relationships.
- `docs/InterfaceSpecifications.md` — subsystem interfaces.
- `docs/archive/` — historical development records.

The earlier *Coding Standards* and *Library Development Standard* documents
remain available in Git history after this consolidation.
