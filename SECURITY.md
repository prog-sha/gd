# Security policy

English | [日本語](SECURITY.ja.md)

## Supported versions

Fixes are included in the latest release. Before the API is stable, safe fixes take priority over compatibility.

## Reporting a vulnerability

Do not disclose vulnerabilities in public issues. Use GitHub's
[private vulnerability reporting](https://github.com/prog-sha/gd/security/advisories/new).
If it is unavailable, open an issue containing no confidential details and ask for a private contact method.

Include the following to help us investigate:

- Affected version, OS, and architecture
- A minimal script and command to reproduce the issue
- Required permission flags and assumptions about the attacker's access
- Expected impact, such as reading, writing, execution, network access, or service interruption
- Your preferred disclosure date, if any

We acknowledge reports within seven days and respond with an assessment and proposed fix.
We coordinate the fix and disclosure date with the reporter, publishing the fixed version, advisory, and credit together.

## Safe use

- Run unverified scripts with `--strict` and grant only the mounts and permissions they need. Named mounts are not supported on Windows; place files in `res://` or `user://` there.
- Native extensions allowed by `--allow-ext` run with the same privileges as gd.
- Keep secrets out of source files, `gd.json`, and command lines. Read them from permitted environment variables.
- Test public servers on loopback first, then grant only the required addresses through `--allow-net`.
- Verify release archives against `SHA256SUMS`.
