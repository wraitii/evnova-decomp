# Pi subagents for EVNova decompilation

The priority is to preserve the main agent's context. Let Pi do the detailed
investigation and implementation; keep the main conversation for direction,
review, user knowledge, and deciding what to do next. A useful partial result
is a starting point for the next iteration.

Use the [runbook](runbook.md) to operate a session and the
[task template](worker-prompt.md) to brief it.

## Drive the work through checkpoints

1. **Identify a feature.** Look around just enough to describe the desired
   behavior, likely entry point, and completion condition. Supply a few known
   paths/addresses and user observations. These are starting points, not a
   prerequisite to map the whole feature yourself.
2. **Let Pi investigate and implement.** It can find the relevant docs, Bible
   sections, Ghidra evidence, dependencies, and tests. Give it room to make a
   coherent improvement. Use a mapping-only task when that is the desired output
   or the user explicitly requested a mapping/approval stage.
3. **Stay involved at useful boundaries.** Read compact status and milestone
   updates while Pi works. Answer questions, challenge assumptions, narrow an
   overlarge plan, and redirect investigation that is not helping the task.
   Inspect a small draft diff when it can catch a structural mistake early.
   Use `steer` for an active session; use `prompt` for an idle session. Do not
   wait for the final summary when a decision or correction is already useful.
4. **Review the result.** Read its short result, the relevant Git diff, and enough
   original evidence to assess important decisions. Check actual validation
   results. Read narrow transcript excerpts only to resolve a specific question;
   do not replay its entire investigation into the main context.
5. **Use the next steps.** Combine Pi's suggested continuation with review findings
   to choose a meaningful follow-up. Reuse an idle session when there is room;
   otherwise send a concise brief to a fresh one. Corrections are a normal part
   of the workflow, not a reason to take all the work back into the main agent.

The main agent drives the task: it owns scope, consequential decisions, review,
and the completion claim. Pi owns the detailed investigation and execution.
Preserving context means reading the evidence needed for decisions, not being
absent until the worker finishes. Avoid both extremes: silently accepting a
summary and replaying every tool call or doing the same investigation twice.

Choose checkpoints for the task rather than imposing an approval ceremony on
every edit. A file split benefits from a proposal before changes: function
groups, resulting file sizes, helper ownership, and new cross-file dependencies.
Review that boundary, then let Pi implement it. During implementation, a shared
header growing into a grab bag is a reason to steer. At completion, check that
the moved bodies, citations, tracker paths, and validation support the result.
A small, well-defined fix can proceed directly to implementation and review.

Ask Pi to report a changed assumption, a blocker, a decision needing input, or
a completed milestone. If progress is unclear, request a short status before
assigning more work. If an active session does not respond to steering, inspect
its compact status and any running tool before deciding to abort. An abort can
leave partial edits or unfinished validation; inspect them before resuming.

## Operating defaults

- One Pi session at a time in the existing checkout. Git carries code changes;
  preserve the starting state and leave commits to the task's authorization.
- Use **DeepSeek V4.1 Flash explicitly**, with a **$0.50 session budget**. Track
  total spending across sessions, including follow-ups and retries.
- Tell it to get going with writing and stopping around **20%** of its 1M context window.
  Finish current edits and validation. A quick follow-up can be done, but avoid
  going beyond 25%.
  Long sessions accumulate input cost as well as context, so a fresh session can be preferable.
- Keep raw tool output and transcripts on disk. Monitor compact status; obtain
  a short result and continuation note with evidence, validation, and next steps.
- Present a normal task conversation. The prompt need not explain the agent
  hierarchy. If asked about the setup, answer truthfully.
- `AGENTS.md` loads automatically and supplies repository rules. Existing images
  in `docs/reference/` are the first source for visual work. If live validation
  is necessary, the main agent discusses it with the user before any probe use.

Distinguish **mapped**, **implemented**, and **validated** behavior. Record Ghidra
mutations because Git cannot show them. These small habits make later iterations
more useful without requiring a large orchestration process.
