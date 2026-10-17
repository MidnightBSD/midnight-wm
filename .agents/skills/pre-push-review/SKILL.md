---
name: pre-push-review
description: Run cppcheck and review outgoing Midnight Workspace changes for basic security issues before pushing this repository. Use when a push is requested or when asked for a pre-push security check.
---

# Pre-push review

From the repository root, run
`./.agents/skills/pre-push-review/scripts/check.sh`. It checks the commits
ahead of the branch's configured upstream with `git diff --check` and runs
the project's cppcheck command over `src/`. The script requires an upstream;
if the branch has none, identify the intended remote branch before reviewing.

Inspect the complete outgoing diff with `git diff @{upstream}...HEAD`. Review
added code and configuration for:

- Credentials, tokens, private keys, or sensitive local paths accidentally
  included in source, tests, examples, or packaging.
- Shell commands and subprocess launches built from desktop entries, filenames,
  D-Bus messages, X11 properties, or other untrusted input. Check quoting,
  argument boundaries, and executable trust decisions.
- File operations that can escape intended directories, follow unexpected
  symlinks, overwrite user data, or leave permissions too broad.
- Ownership, bounds, and lifetime mistakes at XCB, D-Bus, GIO, and C library
  boundaries, including cleanup on error paths.

Check any findings against the actual code and tests; pattern matches alone
are not proof of a vulnerability. Resolve material findings before pushing,
then rerun the checks. Report the cppcheck result, review scope, findings or
remaining uncertainty, and whether the branch is ready to push. This skill
does not authorize a push; push only when the user has requested it.
