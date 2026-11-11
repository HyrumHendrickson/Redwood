# Contributing to Redwood

Thank you for your interest. Please read `AGENTS.md` for the architecture rules and code style, which apply equally to human and automated contributors.

## Getting started

```
git clone https://github.com/HyrumHendrickson/Redwood.git
cd Redwood
make redwood-cli
make test
make check
```

The GUI additionally needs GTK 3 and `pkg-config`; see `README.md`.

## Workflow

1. Open an issue for anything beyond a trivial fix.
2. Fork the repository and create a topic branch from `main`.
3. Make focused commits with imperative-mood subject lines.
4. Add or update tests. Run `make test` and `make check`.
5. Update `README.md` and `CHANGELOG.md` for user-visible changes.
6. Open a pull request and complete the template.

## Reporting bugs

Include the exact command line or GUI settings, the seed, the commit hash, your operating system and compiler, and the observed and expected behaviour. Because runs are deterministic, a seed and settings are usually a complete reproduction.

Security issues follow `SECURITY.md`.

## Licence

By contributing you agree that your contribution is licensed under the MIT licence in `LICENSE`.
