/**
 * Standalone eval harness for compact-smart.
 *
 * Replays a saved pi session against the summarizer prompts without touching the
 * live TUI. Every summarization request sends the WHOLE conversation as prefix
 * (identical for all targets, so provider prompt caching from the original run
 * is fully reused), then a kind-specific instruction, then a restatement of just
 * that one target.
 *
 * Usage:
 *   bun .pi/compact-smart/eval.ts <session.jsonl> [targetNumber]
 *   bun .pi/compact-smart/eval.ts <session.jsonl> --resume <messageIndex> [--no-control] [--no-compact]
 *
 * Omit targetNumber to summarize every eligible segment. Pass one to run a
 * single target while iterating on a prompt.
 *
 * Resume mode is a different experiment: it compacts the history before
 * <messageIndex> and regenerates the assistant message at that index, so the
 * compacted and uncompacted contexts can be compared against each other and
 * against the answer the session actually recorded. Compaction here only sees
 * the prefix, so the future cannot leak into the summaries. The recorded answer
 * is expected to differ (CoT and tool choices are not reproducible); the useful
 * signal is whether the compacted prefix still supports the same answer shape
 * and facts. The session file is never modified.
 *
 * Set PI_PKG to override the pi entry point when it is not at the default
 * global install path.
 */

import {
	CompactionMap,
	contentChars,
	findCompactionTargets,
	formatChars,
	formatTokens,
	messageToText,
	summarizeTargets,
	type CompactionTarget,
} from "../extensions/compact-smart/core";

const PI_PKG =
	process.env.PI_PKG ??
	"/Users/lancelot/.bun/install/global/node_modules/@earendil-works/pi-coding-agent/dist/index.js";

const sessionPath = process.argv[2];
if (!sessionPath) {
	console.error("usage: bun .pi/compact-smart/eval.ts <session.jsonl> [targetNumber]");
	console.error("       bun .pi/compact-smart/eval.ts <session.jsonl> --resume <messageIndex> [--no-control] [--no-compact]");
	process.exit(1);
}

const pi: any = await import(PI_PKG);
const { ModelRuntime, SessionManager, convertToLlm } = pi;

const runtime = await ModelRuntime.create();
const session = SessionManager.open(sessionPath);
const context = session.buildSessionContext();
const messages = context.messages as unknown[];
const conversationChars = messages.reduce((sum, message) => sum + contentChars(message as never), 0);

const modelRef = context.model as { provider: string; modelId: string } | null;
const resolved = modelRef ? runtime.getModel(modelRef.provider, modelRef.modelId) : undefined;
const model = resolved ?? (await runtime.getAvailable())[0];
if (!model) {
	console.error("eval: no available model");
	process.exit(1);
}

const sessionId = session.getSessionId();
const toLlm = (prefix: unknown[]) => convertToLlm(prefix as never) as unknown[];

function charsOf(prefix: unknown[]): number {
	return prefix.reduce((sum, message) => sum + contentChars(message as never), 0);
}

function responseText(response: any): string {
	return response.content
		.map((block: any) => (block.type === "text" ? block.text : ""))
		.filter((text: string) => text.length > 0)
		.join("\n");
}

function makeSummarizer(usages: any[]) {
	return async (request: unknown[]) => {
		const response = await runtime.completeSimple(
			model,
			{ messages: request },
			{ reasoning: "low", temperature: 0.1, sessionId },
		);
		usages.push(response.usage);
		return responseText(response);
	};
}

async function generate(prefix: unknown[]): Promise<{ text: string; usage: any }> {
	const response = await runtime.completeSimple(
		model,
		{ messages: toLlm(prefix) },
		{ reasoning: "low", temperature: 0.1, sessionId },
	);
	return { text: responseText(response), usage: response.usage };
}

function printAnswer(label: string, text: string, prefix: unknown[], usage: any): void {
	const cache = usage ? ` · cache r/w ${usage.cacheRead ?? 0}/${usage.cacheWrite ?? 0}` : "";
	console.log(`\n${"=".repeat(80)}`);
	console.log(
		`${label} · prefix ${formatChars(charsOf(prefix))} · ~${formatTokens(charsOf(prefix))} tok${cache}`,
	);
	console.log("-".repeat(80));
	console.log(text.length > 0 ? text : "(no text content)");
}

// ---------------------------------------------------------------------------
// Resume mode: compact the prefix, then regenerate one message from it.
// ---------------------------------------------------------------------------

function parseResumeIndex(): number | undefined {
	const flag = process.argv.indexOf("--resume");
	if (flag === -1) return undefined;
	const raw = process.argv[flag + 1];
	const index = Number(raw);
	if (!Number.isInteger(index) || index < 0 || index > messages.length) {
		console.error(`eval: --resume expects a message index in [0, ${messages.length}], got '${raw}'`);
		process.exit(1);
	}
	return index;
}

async function runResume(index: number): Promise<void> {
	const prefix = messages.slice(0, index);
	const original = messages[index];
	const compact = !process.argv.includes("--no-compact");
	const withControl = !process.argv.includes("--no-control");

	console.log(`session      ${sessionPath}`);
	console.log(`model        ${model.provider}/${model.id}`);
	console.log(`mode         resume — regenerate message [${index}] from the ${prefix.length} before it`);
	console.log(`prefix       ${formatChars(charsOf(prefix))} chars · ~${formatTokens(charsOf(prefix))} tok`);
	console.log(`recorded     ${original ? messageToText(original as never).slice(0, 160) : "(none — end of session)"}`);
	console.log("");

	if (!compact) {
		const { text, usage } = await generate(prefix);
		printAnswer("generated · uncompacted (--no-compact)", text, prefix, usage);
		return;
	}

	const targets = findCompactionTargets(prefix);
	const map = new CompactionMap();
	map.observe(prefix);
	console.log(`compacting   ${targets.length} targets, from the prefix only...`);
	const usages: any[] = [];
	const results = await summarizeTargets(
		prefix,
		targets,
		toLlm,
		makeSummarizer(usages),
		(done, total, target) => {
			process.stderr.write(`\r[${done}/${total}] ${target.label}          `);
		},
	);
	process.stderr.write("\n");
	for (const result of results) map.applySummary(result.recordKey, result.kind, result.summary);
	const compactedPrefix = map.buildRewritten(prefix, true);

	const stats = map.stats();
	const saved = stats.beforeChars > 0 ? Math.round((1 - stats.afterChars / stats.beforeChars) * 100) : 0;
	console.log(
		`compacted    ${results.length}/${targets.length} segments · ${formatChars(stats.beforeChars)} → ${formatChars(stats.afterChars)} (-${saved}%)`,
	);

	const cache = usages.reduce((sum, usage) => sum + (usage?.cacheRead ?? 0), 0);
	console.log(`cache        ${cache} read tokens across ${usages.length} summaries`);

	if (withControl) {
		const control = await generate(prefix);
		printAnswer("generated · uncompacted control", control.text, prefix, control.usage);
	}

	const { text, usage } = await generate(compactedPrefix);
	printAnswer("generated · compacted", text, compactedPrefix, usage);

	if (original) {
		console.log(`\n${"=".repeat(80)}`);
		console.log("recorded · from session (reference)");
		console.log("-".repeat(80));
		console.log(messageToText(original as never) || "(empty)");
	}
}

const resumeIndex = parseResumeIndex();
if (resumeIndex !== undefined) {
	await runResume(resumeIndex);
	process.exit(0);
}

// ---------------------------------------------------------------------------
// Prompt eval mode: run the summarizer over selected targets.
// ---------------------------------------------------------------------------

const allTargets = findCompactionTargets(messages);
const numbered = allTargets.map((target, index) => ({ target, number: index + 1 }));
const onlyArg = process.argv[3];
const selected = onlyArg ? numbered.filter((entry) => entry.number === Number(onlyArg)) : numbered;

console.log(`session      ${sessionPath}`);
console.log(`model        ${model.provider}/${model.id}`);
console.log(`conversation ${messages.length} messages · ${formatChars(conversationChars)} chars (sent whole for every call)`);
console.log(`targets      ${allTargets.length} eligible segments${onlyArg ? ` · running only #${onlyArg}` : ""}`);
if (selected.length === 0) {
	console.error(onlyArg ? `eval: no target #${onlyArg}` : "eval: no eligible targets");
	process.exit(1);
}
console.log("");
for (const { target, number } of numbered) {
	const marker = selected.some((entry) => entry.number === number) ? ">" : " ";
	console.log(`  ${marker} [${String(number).padStart(2)}] idx ${String(target.index).padStart(2)} ${target.kind.padEnd(10)} ${target.label.padEnd(20)} ${formatChars(target.text.length)}`);
}

const usages: any[] = [];
const results = await summarizeTargets(
	messages,
	selected.map((entry) => entry.target),
	toLlm,
	makeSummarizer(usages),
	(done, total, target) => {
		process.stderr.write(`\r[${done}/${total}] ${target.label}          `);
	},
);
process.stderr.write("\n");

const key = (target: CompactionTarget) => `${target.recordKey}:${target.kind}`;
const summaries = new Map(results.map((result) => [`${result.recordKey}:${result.kind}`, result.summary]));
let totalBefore = 0;
let totalAfter = 0;
selected.forEach((entry, index) => {
	const original = entry.target.text;
	const summary = summaries.get(key(entry.target));
	const after = summary ? summary.length : original.length;
	totalBefore += original.length;
	totalAfter += after;
	const pct = original.length > 0 ? Math.round((1 - after / original.length) * 100) : 0;
	const usage = usages[index];
	const cache = usage ? `  cache r/w ${usage.cacheRead ?? 0}/${usage.cacheWrite ?? 0}` : "";
	console.log(`\n${"=".repeat(80)}`);
	console.log(
		`#${entry.number}/${allTargets.length}  ${entry.target.kind} · ${entry.target.label}  ${formatChars(original.length)} → ${summary ? formatChars(summary.length) : "FAILED"} (-${pct}%)${cache}`,
	);
	console.log("-".repeat(80));
	console.log(summary ?? "(no summary)");
});

if (selected.length > 1) {
	const totalPct = totalBefore > 0 ? Math.round((1 - totalAfter / totalBefore) * 100) : 0;
	console.log(`\n${"=".repeat(80)}`);
	console.log(`total  ${formatChars(totalBefore)} → ${formatChars(totalAfter)} (-${totalPct}%) across ${selected.length} targets`);
}
