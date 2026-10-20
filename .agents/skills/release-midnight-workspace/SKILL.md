---
name: release-midnight-workspace
description: Prepare, publish, and validate a Midnight Workspace GitHub source release and update its bundled MidnightBSD mport. Use when asked to make a release of this repository; do not use for an ordinary version query or unrelated port update.
---

# Release Midnight Workspace

Run this workflow from the repository root. Follow `AGENTS.md` for build, test, packaging, and PR rules. A release request authorizes the release work, but the user chooses the tag.

## Confirm the tag

1. Check the current branch and working tree, fetch `origin` and tags, and inspect the latest published GitHub release, existing tags, `project(... VERSION ...)` in `CMakeLists.txt`, and the bundled port's `PORTVERSION`. Preserve unrelated changes; use a separate worktree if the checkout is busy.
2. **Always ask the user what tag to use before changing the version or creating a tag or release.** Suggest the next numeric release tag by incrementing the last component of the latest published tag: after `v0.1.5`, suggest `v0.1.6`. Mention if `CMakeLists.txt` already names that version. Wait for an explicit answer; a preselected suggestion or elapsed time is not an answer. If the user already answered this question in the current exchange, use that answer without asking again.
3. Verify that the chosen tag is unused on GitHub and that its version matches the intended CMake project version. If it does not, resolve the mismatch with the user before publishing.

## Prepare the release revision

1. Start a focused topic branch from current `origin/master`. Set `project(midnight-wm VERSION ...)` in `CMakeLists.txt` to the chosen version if needed. Search for other release-specific version text; update only references that should describe this release. The bundled mport normally stays on the last *published* version until the new asset exists.
2. Review changes since the previous release and prepare accurate release notes. If installed files or dependencies changed, update `CMakeLists.txt` and the bundled `pkg-plist` together. Compare the bundled mport with `x11-wm/midnight-wm` in MidnightBSD/mports and carry over relevant dependency, `USES`, option, and plist changes. Do not reconstruct its metadata from scratch.
3. Configure a Release build with CMake/Ninja, build it, check `build/midnight-wm --version`, and run `ctest --test-dir build --output-on-failure`. Run the relevant isolated smoke checks from `AGENTS.md` for changes since the prior release. Investigate failures before publishing; record any platform limitation precisely.
4. Commit on the topic branch with DCO sign-off. Run `.agents/skills/pre-push-review/SKILL.md`, including cppcheck and review of the complete outgoing diff. Push, open a PR against `master`, wait for checks, and merge through the PR. Never commit or push directly to `master`. Update the local `master` by fast-forward and confirm the merge revision before packaging.

## Publish the exact source archive

1. From the merged release revision, reconfigure if needed and run `cpack --config build/CPackSourceConfig.cmake -G TGZ`. Confirm the archive name, embedded CMake version, and contents; ensure build output, private files, and generated work directories are excluded. Record its SHA-256 and size.
2. Create the GitHub release with the user-approved tag targeting the **merged revision**, attach that exact `midnight-wm-<version>.tar.gz`, and use reviewed notes. Check that the tag points to the intended commit and the release is published.
3. Download the uploaded asset from GitHub into a clean temporary directory. Compare it byte for byte with the archive just uploaded and record its SHA-256 and size. Treat the downloaded asset as the source of truth for packaging; CPack can produce different bytes on another run.

## Update and validate packaging

1. On a new topic branch from current `origin/master`, update `packaging/mports/x11-wm/midnight-wm/Makefile` `PORTVERSION`, `distinfo` from the downloaded GitHub asset, and the README's bundled-port release reference. Recheck bundled port dependencies, options, and `pkg-plist` against the current official mport. Keep any official MidnightBSD/mports update in its own checkout and PR when that repository is part of the requested release work.
2. Run a clean `bmake -C packaging/mports/x11-wm/midnight-wm -DNO_DEPENDS DISTDIR=<new-temporary-distdir> PACKAGES=<new-temporary-packages-dir> package`. Let it fetch the **published** release archive and verify the checksum. Check the staged binary's version and inspect the produced `.mport`; run CTest on the extracted release source where available. A CMake build or `bmake fake` alone is insufficient.
3. Move generated `_CPack_Packages/`, source tarballs, port `work/`, and `.mport` artifacts out of the checkout before the final status/lint review. Run `portlint -C` with the complete port staged in Git, plus `git diff --check`. Report existing warnings separately from new failures.
4. Commit with DCO sign-off, run the pre-push review skill, push, open a packaging PR against `master`, wait for checks, and merge through the PR. Fast-forward local `master` and verify it is clean, the GitHub release is published, its tag still identifies the release revision, and bundled `distinfo` matches the downloaded asset.

Report the release URL, tag and target commit, asset checksum, PR links, build/test/package results, and any remaining packaging work or limitations. Do not claim the official MidnightBSD/mports port was updated unless its separate change was completed.
