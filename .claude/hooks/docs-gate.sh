#!/usr/bin/env bash
# Repo policy gate (see CLAUDE.md): "always update docs before committing."
#
# Wired as a PreToolUse hook on `git commit` (.claude/settings.json). It blocks
# only when CODE is staged but no docs are — pure-doc and code+doc commits pass.
# It inspects the staged set (`git diff --cached --name-only`), so it assumes the
# normal `git add` → `git commit` flow (not `git commit -a`/`git commit <file>`).
#
# Note: this gates commits made by Claude Code's Bash tool, not your own CLI
# commits.
cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

staged="$(git diff --cached --name-only 2>/dev/null)"
code=$(printf '%s\n' "$staged" | grep -E '^(lib|firmware|desktop)/' || true)
docs=$(printf '%s\n' "$staged" | grep -E '^(README\.md|CLAUDE\.md|docs/)' || true)

if [ -n "$code" ] && [ -z "$docs" ]; then
  cat <<'JSON'
{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"Repo policy (CLAUDE.md): code under lib/, firmware/, or desktop/ is staged but no docs are (README.md, CLAUDE.md, docs/). Update the docs the change affects and `git add` them into this commit, then retry. If a doc update is genuinely not warranted, say so explicitly and re-commit."}}
JSON
fi
exit 0
