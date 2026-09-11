# Running a Pi session

Use [tools/pi_session.mjs](../../tools/pi_session.mjs) from the repository root.
It runs Pi over RPC, stores detailed output on disk, reports compact status, and
keeps completed sessions open for review and follow-ups. Requires Node and Pi.
Only one session should edit the checkout at a time.

Each fresh external-provider session may require explicit user approval because
relevant repository contents can be sent to the configured model provider.

## Prepare and launch

```sh
pi --version
pi --list-models 'deepseek-v4.1-flash'
run_dir=$(mktemp -d /tmp/evnova-pi.XXXXXX)
git rev-parse HEAD > "$run_dir/base-commit.txt"
git status --short > "$run_dir/base-status.txt"
git diff --binary > "$run_dir/base-unstaged.patch"
git diff --cached --binary > "$run_dir/base-staged.patch"
```

Fill [worker-prompt.md](worker-prompt.md) into `task.md` in that directory. Identify
the feature and a few starting points; leave detailed discovery to Pi. Note any
existing changes to preserve. Git patches omit untracked contents, so also retain
a copy of existing untracked files the task may modify. Do not reset or stash as
setup. Use absolute paths in subsequent calls; shell variables may not persist.

```sh
node tools/pi_session.mjs start --run /absolute/run/dir \
  --task /absolute/run/dir/task.md --budget 0.50
```

Run this as a managed long-lived process. The launcher explicitly selects
`openrouter/deepseek/deepseek-v4.1-flash`. Its `--no-extensions --no-skills
--no-prompt-templates` flags **preserve `AGENTS.md` loading**; never add
`--no-context-files`. Pi needs its existing provider credentials and access to its
configuration lock files. Use the host's approval mechanism if that access is
blocked; do not print secrets.

## Monitor without filling the main context

```sh
node tools/pi_session.mjs status --run /absolute/run/dir
```

The launcher polls usage every 30 seconds and prints a heartbeat each minute.
Raw events and stderr stay in `events.jsonl` and `stderr.log`. Read only relevant
excerpts when diagnosing a problem. When idle, read the indicated `result-N.md`
and, as needed, the continuation note and Git diff.

Default budget: **$0.50**, reminder at **$0.375**. Context: wind down at **20%**,
no new work at **30%**. The launcher sends a natural context reminder and rejects
new follow-up prompts at 30%. Cost is cumulative per session; never sum successive
status snapshots. Keep total task cost across fresh sessions. Report unknown
usage as unknown, not zero.

These are supervisory limits, not provider-enforced caps: usage can arrive only
after a response. The launcher stops inference at the reported budget boundary,
allowing a detected running build/test to finish. Inspect partial work after an
abort. Do not use compaction to justify another long session.

## Review and continue

Review `git status --short`, `git diff`, and `git diff --cached`, comparing with
the saved baseline. Include untracked files and any authorized commits since
the starting commit. Check the result's evidence and validation, then choose the
next step; don't automatically repeat successful checks on unchanged code.
Run builds and tests from the repository root; resource-backed tests may locate
the shipped archives relative to it. Report whether those tests ran or skipped.

For cross-layer changes, verify resource selection, frames, anchors, and reset
state agree between simulation and rendering. If a field's meaning changes,
audit every reader and writer. Search changed docs, trackers, and TODOs for stale
names or "unported" claims before handoff.

For a focused follow-up with enough context and budget, write a short task file:

```sh
node tools/pi_session.mjs prompt --run /absolute/run/dir --task /absolute/follow-up.md
```

Use `steer` instead of `prompt` to correct ongoing work. Commands are queued;
check the managed process output/status for acceptance. At a context boundary,
close the idle session and start a fresh directory with the continuation note
and review findings as the brief. Do not carry the raw transcript forward.
A single narrow correction pass may continue to about 25% context; after that,
finish review locally or start a fresh session.

```sh
node tools/pi_session.mjs close --run /absolute/run/dir
```

`close` refuses a busy session; `abort` explicitly interrupts one. Keep durable
findings in subsystem docs and preserve needed continuation notes before temporary
files are cleaned up. Git remains the record of implementation changes.

## Setup reference

Checked against Pi 0.85.1. The model was available locally through OpenRouter
with a 1,048,576-token window. A live smoke test verified model selection,
`AGENTS.md` loading, tool use, steering, and usage reporting. Check availability
again on another installation. If missing, register it in `~/.pi/agent/models.json`
using installed `docs/models.md` and the [provider catalog](https://openrouter.ai/api/v1/models);
do not edit the generated model cache. Rates and availability can change.

For custom orchestration, consult Pi's installed `docs/rpc.md` or the
[RPC reference](https://github.com/earendil-works/pi-mono/blob/main/packages/coding-agent/docs/rpc.md).
JSON print mode and the example subagent extension are alternatives, but do not
provide this launcher's persistent review/follow-up control by themselves.
