# Pi subagents

Use Pi with DeepSeek like a Luna subagent: give it a concise task, let it work,
steer when useful, and review the result. Keep detailed investigation in its
session instead of duplicating it in the main conversation.

Pi-specific defaults:

- Run one session at a time in the current checkout through
  [tools/pi_session.mjs](../../tools/pi_session.mjs).
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

See the [runbook](runbook.md) for commands.
