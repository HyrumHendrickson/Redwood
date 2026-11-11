# Security Policy

## Scope

Redwood is a simulation and teaching tool. It does not hold funds, connect to any real cryptocurrency network, or implement production cryptography. In particular, signatures are a simulation oracle (`SHA-256(secret || message)` checked internally) and must not be used to protect real assets.

Security reports are still welcome for issues such as:

- memory-safety defects (buffer overflows, out-of-bounds access, use after free, integer overflow leading to any of these)
- crashes or hangs reachable through command-line arguments or GUI input
- flaws in the SHA-256 implementation
- weaknesses in the build or CI configuration, such as unsafe workflow permissions

## Supported versions

Only the latest commit on `main` is supported.

## Reporting a vulnerability

Please do not open a public issue for a suspected vulnerability.

Use GitHub private vulnerability reporting: open the repository's **Security** tab and choose **Report a vulnerability**. Include:

- a description of the issue and its impact
- steps or a command line that reproduces it
- the commit hash, operating system and compiler version

You can expect an acknowledgement within 7 days and a status update within 30 days. Credit is given in the changelog unless you ask otherwise.

## Hardening in this repository

- Builds use `-D_FORTIFY_SOURCE=2` and `-fstack-protector-strong`.
- CI runs the test suite under AddressSanitizer and UndefinedBehaviorSanitizer.
- CodeQL analyses every push and pull request.
- Dependabot keeps GitHub Actions versions current.
- Command-line input is validated and rejected with exit status 2 when malformed.
