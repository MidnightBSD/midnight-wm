#!/bin/sh
set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}') || {
    echo 'No upstream branch is configured; identify the intended push target first.' >&2
    exit 1
}

echo "Reviewing commits ahead of $upstream"
git diff --check "$upstream"...HEAD
git diff --stat "$upstream"...HEAD

cppcheck --enable=warning,performance,portability --std=c++20 \
    --suppress=missingIncludeSystem --error-exitcode=1 --quiet \
    '-DMIDNIGHT_WM_VERSION="dev"' src

echo 'Diff validation and cppcheck passed. Complete the security review in SKILL.md before pushing.'
