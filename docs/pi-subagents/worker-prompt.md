# EVNova task

Copy and fill this prompt. Remove instructional placeholders before dispatch.
Remove this introductory paragraph too. Omit unused fields; link evidence rather
than embedding large dumps. Repository rules are already loaded from `AGENTS.md`.

## Assignment

- Task: <one concrete behavior and completion condition>
- Mode: <implement | map; include any user-requested approval gate>
- Checkpoints: <only decisions worth reviewing before proceeding; omit for a
  straightforward task that can go directly to implementation>
- Workspace: <absolute checkout path>
- Git baseline: <starting commit; existing changes to preserve, or clean>
- Concurrent work: <other active writers and exact owned paths, if any>
- Out of scope: <adjacent behaviors not included in this task>
- Validation focus: <task-specific cases beyond the standard AGENTS.md checks>
- Commit: <leave changes uncommitted, unless committing is already authorized>
- Continuation note: <absolute writable path outside unrelated source files>
- Budget: <$0.50 per session unless specified otherwise; total task budget if supplied>

## Starting evidence

- Current state: <what already works; what is actually missing>
- Start here: <a few known source/doc paths or original addresses; no exhaustive map needed>
- References: <Bible topic, relevant images in docs/reference/, saved decompile excerpts>
- User observations: <specific correction/recollection and confidence>
- Known uncertainty: <the interpretation that most needs checking>

## Working agreement

Follow the repository instructions in `AGENTS.md`; they supply the decompilation,
Ghidra, coding, tracker, probe, and validation rules. Take the feature through
investigation, implementation, and validation unless this is a mapping-only task.
Choose your own approach. The starting evidence is intentionally brief: find the
relevant subsystem docs, Bible sections, callers, helpers, layouts, and tests as
needed. Check the current source; tracker prose and older notes can be stale.

Work toward a coherent improvement rather than trying to finish every adjacent
dependency. If the feature turns out larger than expected, complete a useful
slice, record what remains, and recommend the next step. If it turns out small,
finish it and identify a useful follow-up rather than silently expanding scope.
Keep evidence and unresolved interpretations distinct, especially where decompiler
types or names are misleading. Tests should exercise known behavior and important
edge cases, not merely confirm the chosen implementation.

Use Git to review your changes against the supplied baseline, including staged
and untracked files. Preserve pre-existing changes and keep edits relevant to
the task. Respect the assigned file ownership if other work is active; do not
assume every new diff belongs to this session or restore from an older snapshot
over another writer's changes. Work in this session
without launching additional agents. For visual work, start with screenshots in
`docs/reference/` and actually inspect the relevant images. Prefer existing
references to operating the probe. If live validation is necessary, explain the
specific question and needed observation so we can discuss it before using the
probe. Continue useful static work and mark unverified appearance/timing as pending.

Keep this session focused. Around 20% of the 1M context window (about 200k tokens),
start winding down; finish the current changes and validation rather than opening
another investigation. At 30%, do not start new work: save a continuation note
and wrap up. Do not use compaction to reset these limits. Respect budget reminders;
if usage or cost measurements are unavailable, do not invent them.

Complete the repository's required validation and the task-specific cases above.
Run validation from the repository root. For resource-backed tests, report
whether they executed against the archives or skipped.
Mark unfinished checks as pending. If blocked, explain the evidence and smallest
missing decision; continue useful in-scope work that does not depend on it.

Keep progress messages brief: an important finding, a decision needing input, or
the current milestone. Surface changed assumptions and scope/dependency growth
while they are actionable, rather than saving them for the final result. At an
explicit proposal checkpoint, return the concrete proposal and wait for the
follow-up. Otherwise continue within the assigned scope. Answer status requests
with completed work, the current step, and any blocker; then resume.
Store long decompiles and diagnostics in files and read
targeted portions. Preserve complete build logs and exit statuses without piping
builds through `head`/`tail`. Patch only affected tracker rows. Ensure newly
documented Ghidra behavior, tracker claims, and actual code agree before wrapping up.
If evidence changes a field's meaning, audit every reader/writer and remove stale
names. For cross-layer work, verify resource, frame, anchor, and reset state agree,
then search changed docs and TODOs for superseded claims.

## Result and continuation note

Return a short result (roughly 150–250 words) and the continuation note path. Give
the behavior changed, validation status, remaining gap, and recommended next step.
Do not repeat the whole investigation or paste code diffs into the final response.

The continuation note can contain the additional detail needed to resume:

- Outcome: what is mapped, implemented, and validated (separately).
- Evidence: original addresses, confirmed quirks, and unresolved interpretations.
- Changes: brief behavior summary and any commits; Git carries the code diff.
- Ghidra: each mutation by address/type, since these changes are outside Git.
- Validation: exact commands and results; checks or visual comparisons still pending.
- Remaining work: one recommended next task, why it matters, starting paths/
  addresses, useful alternatives, and any blocker. Include review questions that
  would help distinguish a sound reconstruction from a plausible approximation.

Save durable discoveries in the relevant subsystem note. Make it possible to
resume from this note without reconstructing the entire session.
