# Contributing

English | [日本語](CONTRIBUTING.ja.md)

This repository contains public product source and user documentation. Development tools, internal tests, and work records are maintained separately.

The bundled [smoke tests](tests/README.md) run with only the public source and executable. Use them to validate proposed fixes.

Report bugs and suggestions through [Issues](https://github.com/prog-sha/gd/issues). Include the version, OS, a minimal reproducer, and expected behavior. Proposed fixes can be submitted as source diffs; maintainers validate them in the development repository before integration.

For vulnerabilities, follow the [security policy](SECURITY.md) instead of opening a public issue. See [README](README.md#build-from-source) for build instructions and the [code of conduct](CODE_OF_CONDUCT.md) for participation guidelines.

Stable releases use a major.minor branch such as `0.8`. Tags follow SemVer: `0.8.0` for a stable release, and `0.8.0-dev.1` or `0.8.0-rc.1` for development and candidate builds. They carry no `v` prefix and no `-stable` suffix. Published tags remain immutable. Public Actions run native checks manually.

Target pull requests at `master`, the development branch for the next minor version. Use `0.8` for stable source and maintenance. Patch versions share that branch; do not create patch branches.
