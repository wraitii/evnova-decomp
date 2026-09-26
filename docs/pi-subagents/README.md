# Pi subagents

Use Pi with DeepSeek like a Luna subagent: give it a concise task, let it work,
steer when useful, and review the result. Keep detailed investigation in its
session instead of duplicating it in the main conversation.

Pi-specific defaults:

- Run sessions through [tools/pi_session.mjs](../../tools/pi_session.mjs). If running multiple in the checkout, just warn them of each other, it's fine.
- The launcher selects `openrouter/deepseek/deepseek-v4.1-flash`. Use a **$0.50
  session budget** and track cumulative cost across retries and new sessions.
- `AGENTS.md` loads automatically. No prompt template is needed: include the
  task, useful starting points, and any task-specific constraints.
- Ask it to start wrapping up around **20% of its 1M context window**; keep a
  narrow follow-up below about 25%. Start fresh when needed.
- Sessions remain open for follow-ups. Detailed output stays on disk; use compact
  status and targeted excerpts to guide and review the work.
- Pi uses external provider credentials and sends task/repository context to
  DeepSeek through OpenRouter. Request host approval if access is blocked.

## Quirks of Deepseek v4.1 flash

While an extremely competent model, it has some quirks that are worth keeping an eye on when steering:
- Tendency to over-explore. It can be worth telling it explicitly to start working after 10-15% context use. Also potentially worth inspecting the tool-call reads to make sure it's understood the scope properly.
- It's good at decompilation but on large functions can get a few things confused, worth asking questions to settle facts sometimes.
- Prefer asking it to investigate without edits first, then decide on edits, as it's quite eager to tweak things.

# Pi session commands

Run from the repository root. Requires Node, Pi, and configured OpenRouter
credentials. The launcher selects DeepSeek V4.1 Flash and preserves `AGENTS.md`
loading while disabling extensions, skills, and prompt templates.

## Start

Create a temporary run directory and write a short, task-specific prompt to
`task.md`. Record the starting Git state so you can distinguish its changes from
existing work. Leave commits unmade unless authorized.

```sh
run_dir=$(mktemp -d /tmp/evnova-pi.XXXXXX)
git rev-parse HEAD > "$run_dir/base-commit.txt"
git status --short > "$run_dir/base-status.txt"
git diff --binary > "$run_dir/base-unstaged.patch"
git diff --cached --binary > "$run_dir/base-staged.patch"
# Write your prompt to "$run_dir/task.md", then:
node tools/pi_session.mjs start --run "$run_dir" \
  --task "$run_dir/task.md" --budget 0.50
```

Keep `start` running as a managed process. Use the absolute run path in later
calls; shell variables may not persist. If credential/configuration lock access
is blocked, use host approval; don't print credentials or bypass a rejection.
Approval may explicitly need to cover sending repository contents to the provider.

## Monitor and guide

Every request is acknowledged in `status.json` under `lastRequest`, so a caller
that only sees files can confirm delivery without reading the supervisor's
stdout. Commands exit `0` accepted, `1` refused/usage error, `2` no
acknowledgement, `3` budget reached, `4` failed, `5` launcher gone. Add `--json`
for one machine-readable object on stdout (human output otherwise; chatter goes
to stderr).

```sh
node tools/pi_session.mjs status --run /absolute/run/dir --json
node tools/pi_session.mjs talk   --run /absolute/run/dir --message 'prefer the monotonic clock'
node tools/pi_session.mjs wait   --run /absolute/run/dir --timeout 300
```

`talk` is the usual way to speak to the session: it prompts when idle and
steers when a turn is running, decided atomically against the live state.
`steer` alone is refused unless running; `prompt` (or `talk`) is refused unless
idle. Messages can come from `--message TEXT`, `--task FILE`, or `--task -`
(stdin). `--ack-timeout 0` queues and returns immediately.

`wait` blocks until the session settles (`agent_settled`) or the timeout
expires, then prints the result path and cost; it is the single-call alternative
to polling `status`. `status` reports state, cost, context, last tool, the
latest `result-N.md`, and the last request's acceptance. It exits `5` when the
launcher is gone but the state still says running. Raw events are in
`events.jsonl`; session transcripts are under `sessions/`. The launcher polls
usage every 30 seconds and prints a heartbeat each minute.

Cost is cumulative per session: don't add successive status snapshots. It reminds
at $0.375 and stops inference at the reported $0.50 boundary, allowing a detected
build/test to finish. Usage can arrive late, so this isn't a hard provider cap.
It reminds at 20% context and rejects new turns at 30%; aim to wrap up earlier.

## Review and close

Read the result, relevant diff, and validation evidence. Use the same idle
session for focused corrections when budget/context allow. Ask for a short
continuation note when handing unfinished work to a fresh session.

```sh
node tools/pi_session.mjs close --run /absolute/run/dir
node tools/pi_session.mjs abort --run /absolute/run/dir
```

`close` requires an idle session; `abort` stops the current turn and returns to
idle. Inspect partial edits and unfinished checks before resuming.
