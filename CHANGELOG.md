# Changelog

All notable changes are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- `AGENTS.md`, `SECURITY.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` and this changelog.
- Regression test suite (`make test`) and sanitizer build (`make check`).
- GitHub Actions CI, CodeQL analysis, Dependabot, issue and pull request templates, and `CODEOWNERS`.
- `.editorconfig`, `.clang-format` and `.gitattributes`.

### Changed
- Builds now use `-D_FORTIFY_SOURCE=2` and `-fstack-protector-strong`.
- Allocation failures in the simulation core now abort with a message instead of dereferencing `NULL`.

### Fixed
- `redwood-cli` crashed with a segmentation fault on a negative coin argument.
- `redwood-cli` silently accepted malformed numeric arguments, out-of-range node indices and cheat masks. It now prints usage and exits with status 2.
