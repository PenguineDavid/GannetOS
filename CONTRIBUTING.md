# Contributing to GannetOS

Thanks for your interest in GannetOS! This document explains how to contribute and what we're building.

## The Vision

GannetOS is a hobby OS inspired by Linux's design philosophy but built to deliberately improve upon it. The idea is not trying to clone Linux - it's to learn from it and build a cleaner, more intentional foundation. The goal is a real, stable, open-source OS that prioritizes:

- **Clean fundamentals** instead of premature features
- **Process isolation and memory safety** as core properties
- **Community-driven development** with room for different architectural ideas
- **Educational value** - code should be readable and teach OS concepts

## Current Status

**GannetOS is in early stabilization phase.** If you give a random PR to add some cool feature I probably won't merge it. If you give me a way to optimize the internal memory logic or fix a paging bug, that I will accept after a quick review. Don't expect the review to be that fast though, my timezone is ASET.

This means:
- Contributing to priority items below is very welcome
- Please don't add new subsystems or experimental features yet
- Bug fixes, tests, documentation, and code cleanup always help
- Design discussions are encouraged - open an issue first or start a repo conversation

## What We Need Right Now

Prioritized by impact:

1. **Virtual memory & paging** - page tables, user/kernel split, page fault handling (this will actually hopefully be gone by the time I make this repo public but if you're seeing this I didn't finish it).
2. **Process isolation** - task-local address spaces, cleanup on exit
3. **Context switch robustness** - register preservation, interrupt safety during switches
4. **Error handling** - crash recovery, process cleanup, pipe/file handle cleanup
5. **Filesystem stability** - path normalization edge cases, symlink handling
6. **Documentation** - architecture docs, code comments, design explanations
7. **Tests** - stress tests, edge case coverage, crash recovery verification

## How to Contribute

### Before you start

1. Check [TODO.md](TODO.md) for priority areas
2. If it IS a new feature, open an issue first to discuss. Don't surprise me with a huge PR.
3. For bug fixes or priority items, go ahead and open a PR.

### Code style

- Follow the existing C conventions in the codebase
- Comments should explain *why*, not just *what* and mark them with the date and your name (e.g. // TODO: make the code better - David 31/08/2026)
- Keep kernel code focused and tight - every line of kernel code matters
- Use meaningful variable names; avoid excessive macros

### License headers and attribution

- Source files in this repository use the short SPDX header identifying the project copyright and GPLv3 license.
- Contributors do not need to add a new copyright line every time they edit an existing file.
- A contributor may add their name to a new file or a substantial standalone contribution when they want attribution, but this is optional and should not replace the GPLv3 SPDX identifier.
- Do not copy LGPL, MIT, or library-specific boilerplate into GannetOS files. Contributions must remain compatible with the repository's GPLv3 license.

### Commit messages

- Clear, concise first line (50 chars or less)
- If needed, explain the context and reasoning in the body
- Reference issue numbers if applicable

### Testing

- Test your changes in QEMU
- For kernel changes, verify the build succeeds and the OS still boots
- Add a comment in your PR describing how you tested it

### PR process

1. Fork and create a feature branch: `git checkout -b feature/your-feature`
2. Make your changes
3. Test in QEMU (see [Makefile](Makefile) for build targets)
4. Push to your fork and open a PR against `main`
5. Include:
   - What problem you're solving
   - How you tested it
   - Any design decisions you made
   - Known limitations or edge cases

## License

All contributions must be compatible with **GPLv3**. By contributing, you agree your code will be distributed under this license.

## Questions?

- Check [kernel/](kernel/) and [shell/](shell/) headers for API documentation
- Look at existing code for patterns and examples
- Open an issue if something is unclear

Thanks for helping build GannetOS.
