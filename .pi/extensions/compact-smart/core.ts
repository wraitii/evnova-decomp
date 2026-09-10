/**
 * Pure core for the compact-smart experiment.
 *
 * Shared by the pi extension (`.pi/extensions/compact-smart/index.ts`) and the
 * standalone eval harness (`.pi/compact-smart/eval.ts`). It must not import any
 * pi runtime module, so it can run both inside pi/jiti and under plain bun.
 *
 * Responsibilities: message identity, size accounting, the 1:1 record map, the
 * per-kind compaction prompts, and the apply transform. The LLM call itself
 * lives behind an injected function so this file stays dependency-free.
 */

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** Do not bother summarizing segments smaller than this. */
export const MIN_TARGET_TOKENS = 200;

/** The kinds of content we compact, each with its own prompt. */
export type TargetKind = "toolResult" | "user" | "thinking" | "assistant";

const COMPACT_PREAMBLE = [
	"-----",
	"The conversation above is a long agent session.",
	"This message is part of an automated compaction process.",
	"This compaction process rewrites each message and tool call to be shorter while preserving structure.",
	"The excerpt below is a restatement of an earlier message in the conversation, for reference.",
].join(" ");

/**
 * Per-kind compaction instructions, appended to COMPACT_PREAMBLE.
 *
 * toolResult and assistant favor a dense factual summary over a structural
 * copy: identifiers, paths and addresses survive, but bodies and filler do not.
 *
 * The thinking prompt deliberately relies on the assistant's visible reply
 * (and later messages) carrying the conclusions, so it emits only the deltas a
 * reader could not recover from the rest of the transcript. This couples it to
 * the assistant prompt: if the visible reply is later compacted more lossily
 * than this prompt assumes, reasoning can be dropped that nothing else
 * restates. `/compact-smart clear` restores any one message's original
 * segments when that happens.
 */
const KIND_INSTRUCTIONS: Record<TargetKind, string> = {
	toolResult: [
		"This target is a tool call result.",
		"Rewrite it as a dense summary instead of a copy: state what the tool did and everything",
		"later steps depend on — file/symbol names, line ranges, discovered facts, failures.",
		"Do not reproduce whole code blocks; cite the top-level declaration and its role.",
		"Keep it under a third of the original.",
		"Output only the compacted text, with no preamble.",
	].join(" "),
	user: [
		"This target is a user message.",
		"Rewrite it shorter while preserving the user's intent, constraints, and any concrete",
		"instructions, questions, paths, or values. Never change the meaning and never add requirements.",
		"Output only the rewritten text, with no preamble.",
	].join(" "),
	thinking: [
		"This target is the assistant's internal reasoning.",
		"Its conclusions are restated in the assistant's visible reply and later messages, so do not repeat them.",
		"Output exactly one of the following and nothing else.",
		"(a) If the reasoning adds nothing beyond those visible messages, output only [..reasoning skipped..].",
		"(b) Otherwise output a few short bullet points naming only open questions, rejected alternatives,",
		"uncertainty, or facts that no visible message states; this must be significantly shorter than the original.",
		"No preamble.",
	].join(" "),
	assistant: [
		"This target is the assistant's reply to the user.",
		"Shorten it as much as possible while keeping every concrete claim, number, address, file",
		"path and next step. Prefer bullets over paragraphs and drop explanation of things already stated.",
		"Output only the condensed reply, with no preamble.",
	].join(" "),
};

export function instructionFor(kind: TargetKind): string {
	return `${COMPACT_PREAMBLE} ${KIND_INSTRUCTIONS[kind]}`;
}

/** How a message is planned to appear in the compacted view. */
export type Disposition = "keep" | "trim" | "summarize" | "drop";

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

export function clamp(value: number, min: number, max: number): number {
	return Math.max(min, Math.min(max, value));
}

export function formatChars(chars: number): string {
	if (chars < 1000) return `${chars}c`;
	if (chars < 1_000_000) return `${(chars / 1000).toFixed(1)}k`;
	return `${(chars / 1_000_000).toFixed(1)}M`;
}

/** Rough token estimate shared across the widget and the viewer. */
export function estimateTokens(chars: number): number {
	return Math.ceil(chars / 4);
}

export function formatTokens(chars: number): string {
	return formatChars(estimateTokens(chars));
}

export function firstLinePreview(text: string, max: number): string {
	const line = text.split("\n").find((l) => l.trim().length > 0) ?? "";
	const flat = line.trim().replace(/\s+/g, " ");
	return flat.length > max ? `${flat.slice(0, max - 1)}…` : flat;
}

// ---------------------------------------------------------------------------
// Message inspection helpers
// ---------------------------------------------------------------------------

export type LooseMessage = {
	role?: string;
	timestamp?: number;
	toolCallId?: string;
	toolName?: string;
	customType?: string;
	isError?: boolean;
	content?: unknown;
	summary?: string;
	command?: string;
	output?: string;
};

export function toolCallIds(message: LooseMessage): string[] {
	if (!Array.isArray(message.content)) return [];
	const ids: string[] = [];
	for (const block of message.content as Array<Record<string, unknown>>) {
		if (block && block.type === "toolCall" && typeof block.id === "string") {
			ids.push(block.id);
		}
	}
	return ids;
}

export function toolCallNames(message: LooseMessage): string[] {
	if (!Array.isArray(message.content)) return [];
	const names: string[] = [];
	for (const block of message.content as Array<Record<string, unknown>>) {
		if (block && block.type === "toolCall" && typeof block.name === "string") {
			names.push(block.name);
		}
	}
	return names;
}

/**
 * Stable identity for a context message. `context` hands us fresh deep copies
 * every turn with no entry ids, so the key must be derived from immutable
 * fields. Tool results and tool-calling assistants key off their unique ids;
 * other roles fall back to the timestamp, which is unique per logical message
 * in practice.
 */
export function messageKey(message: LooseMessage): string {
	const role = message.role ?? "unknown";
	const stamp = message.timestamp ?? "?";
	switch (role) {
		case "toolResult":
			return `tr:${message.toolCallId ?? stamp}`;
		case "assistant": {
			const ids = toolCallIds(message);
			return ids.length > 0 ? `as:${ids.join(",")}` : `as:${stamp}`;
		}
		case "user":
			return `us:${stamp}`;
		case "custom":
			return `cu:${message.customType ?? "?"}:${stamp}`;
		case "bashExecution":
			return `bx:${stamp}`;
		case "compactionSummary":
			return `cs:${stamp}`;
		case "branchSummary":
			return `bs:${stamp}`;
		default:
			return `${role}:${stamp}`;
	}
}

export function messageLabel(message: LooseMessage): string {
	const role = message.role ?? "unknown";
	switch (role) {
		case "assistant": {
			const names = [...new Set(toolCallNames(message))];
			return names.length > 0 ? `assistant → ${names.join(",")}` : "assistant";
		}
		case "toolResult":
			return `result ← ${message.toolName ?? "?"}${message.isError ? " (error)" : ""}`;
		case "bashExecution":
			return "bash execution";
		case "custom":
			return `custom:${message.customType ?? "?"}`;
		case "compactionSummary":
			return "compaction summary";
		case "branchSummary":
			return "branch summary";
		default:
			return role;
	}
}

function blockChars(block: Record<string, unknown>): number {
	if (typeof block.text === "string") return block.text.length;
	if (typeof block.thinking === "string") return block.thinking.length;
	if (block.arguments !== undefined) {
		try {
			return JSON.stringify(block.arguments).length;
		} catch {
			return 0;
		}
	}
	if (typeof block.data === "string") return block.data.length;
	return 0;
}

export function contentChars(message: LooseMessage): number {
	if (typeof message.content === "string") return message.content.length;
	if (Array.isArray(message.content)) {
		let total = 0;
		for (const raw of message.content as unknown[]) {
			if (raw && typeof raw === "object") total += blockChars(raw as Record<string, unknown>);
		}
		return total;
	}
	if (typeof message.summary === "string") return message.summary.length;
	if (typeof message.output === "string") return message.output.length;
	return 0;
}

function contentToText(content: unknown): string {
	if (typeof content === "string") return content;
	if (!Array.isArray(content)) return "";
	const parts: string[] = [];
	for (const raw of content as Array<Record<string, unknown>>) {
		if (!raw || typeof raw !== "object") continue;
		if (raw.type === "text" && typeof raw.text === "string") parts.push(raw.text);
		else if (raw.type === "thinking" && typeof raw.thinking === "string") {
			parts.push(`[thinking] ${raw.thinking}`);
		} else if (raw.type === "toolCall") {
			const args = raw.arguments !== undefined ? JSON.stringify(raw.arguments) : "{}";
			parts.push(`[tool call ${String(raw.name ?? "?")} ${args}]`);
		} else if (raw.type === "image") {
			parts.push(`[image ${String(raw.mimeType ?? "?")}]`);
		}
	}
	return parts.join("\n");
}

/** Readable plain-text form used for size accounting and the viewer. */
export function messageToText(message: LooseMessage): string {
	const role = message.role ?? "unknown";
	switch (role) {
		case "bashExecution":
			return `$ ${message.command ?? ""}\n${message.output ?? ""}`;
		case "compactionSummary":
			return `[compaction summary]\n${message.summary ?? ""}`;
		case "branchSummary":
			return `[branch summary]\n${message.summary ?? ""}`;
		default:
			return contentToText(message.content);
	}
}

function blockText(block: Record<string, unknown>): string {
	if (typeof block.text === "string") return block.text;
	if (typeof block.thinking === "string") return block.thinking;
	return "";
}

function blocksOfType(content: unknown, type: string): string[] {
	if (!Array.isArray(content)) return [];
	const texts: string[] = [];
	for (const raw of content as Array<Record<string, unknown>>) {
		if (raw && raw.type === type) {
			const text = blockText(raw);
			if (text) texts.push(text);
		}
	}
	return texts;
}

/**
 * Extract the compactable segments of a message. Only single-block kinds are
 * targeted, so applying a summary back is unambiguous. Assistant tool-call
 * blocks are never targeted and are preserved on rewrite.
 */
function extractSegments(message: LooseMessage): RecordSegment[] {
	const role = message.role ?? "";
	const segments: RecordSegment[] = [];
	const push = (kind: TargetKind, text: string) => {
		if (text.length > 0) segments.push({ kind, originalText: text, mappedText: text, compacted: false });
	};
	if (role === "toolResult") {
		const texts = blocksOfType(message.content, "text");
		if (texts.length === 1) push("toolResult", texts[0]);
	} else if (role === "user") {
		if (typeof message.content === "string") push("user", message.content);
		else {
			const texts = blocksOfType(message.content, "text");
			if (texts.length === 1) push("user", texts[0]);
		}
	} else if (role === "assistant") {
		const thinking = blocksOfType(message.content, "thinking");
		if (thinking.length === 1) push("thinking", thinking[0]);
		const texts = blocksOfType(message.content, "text");
		if (texts.length === 1) push("assistant", texts[0]);
	}
	return segments;
}

function segmentLabel(base: string, kind: TargetKind): string {
	switch (kind) {
		case "toolResult":
			return base;
		case "user":
			return "user message";
		case "thinking":
			return "assistant reasoning";
		case "assistant":
			return "assistant response";
	}
}

// ---------------------------------------------------------------------------
// Compaction map
// ---------------------------------------------------------------------------

export interface RecordSegment {
	kind: TargetKind;
	originalText: string;
	mappedText: string;
	compacted: boolean;
}

export interface MapRecord {
	key: string;
	/** Last observed position in the outgoing context. */
	index: number;
	role: string;
	label: string;
	disposition: Disposition;
	originalChars: number;
	mappedChars: number;
	originalText: string;
	mappedText: string;
	note: string;
	/** Per-segment originals and (once summarized) their replacements. */
	segments: RecordSegment[];
	/** True once any segment has been summarized. */
	compacted: boolean;
	/** Whether the message was present in the most recent observed context. */
	inContext: boolean;
	firstTurn: number;
	lastTurn: number;
	seen: number;
}

/**
 * A single unit to summarize: one compactable segment of one message. `text`
 * is the exact original content, used for the token floor and the restatement.
 */
export interface CompactionTarget {
	recordKey: string;
	/** Index of the message in the conversation. */
	index: number;
	kind: TargetKind;
	label: string;
	text: string;
}

/** Find in-context segments worth compacting, in conversation order. */
export function findCompactionTargets(messages: unknown[]): CompactionTarget[] {
	const targets: CompactionTarget[] = [];
	messages.forEach((raw, index) => {
		const message = (raw ?? {}) as LooseMessage;
		const recordKey = messageKey(message);
		const baseLabel = messageLabel(message);
		for (const segment of extractSegments(message)) {
			if (estimateTokens(segment.originalText.length) < MIN_TARGET_TOKENS) continue;
			targets.push({
				recordKey,
				index,
				kind: segment.kind,
				label: segmentLabel(baseLabel, segment.kind),
				text: segment.originalText,
			});
		}
	});
	return targets;
}

/**
 * The target content repeated after the instruction, clearly labelled as a
 * restatement rather than new conversation.
 */
export function buildRestatement(target: CompactionTarget): string {
	return [
		`Restatement — the exact ${target.label} to shorten, repeated from above:`,
		"",
		target.text,
	].join("\n");
}

/** Converts AgentMessages to provider messages (pi's convertToLlm, injected). */
export type ToLlmMessages = (messages: unknown[]) => unknown[];
/** Runs one summarization request and returns the assistant text. */
export type CompleteOne = (messages: unknown[]) => Promise<string>;

export interface SummaryResult {
	recordKey: string;
	kind: TargetKind;
	originalChars: number;
	summary: string;
}

/**
 * Summarize targets one at a time. Every request contains the WHOLE
 * conversation as prefix (identical for every target, so provider prompt
 * caching is fully reused), then the kind-specific instruction and a
 * restatement of just that target at the very end.
 */
export async function summarizeTargets(
	messages: unknown[],
	targets: CompactionTarget[],
	toLlm: ToLlmMessages,
	complete: CompleteOne,
	onProgress?: (done: number, total: number, target: CompactionTarget) => void,
): Promise<SummaryResult[]> {
	const results: SummaryResult[] = [];
	let done = 0;
	for (const target of targets) {
		const request = [
			...toLlm(messages),
			{
				role: "user",
				content: [
					{ type: "text", text: instructionFor(target.kind) },
					{ type: "text", text: buildRestatement(target) },
				],
				timestamp: Date.now(),
			},
		];
		const summary = (await complete(request)).trim();
		if (summary) {
			results.push({
				recordKey: target.recordKey,
				kind: target.kind,
				originalChars: target.text.length,
				summary,
			});
		}
		done += 1;
		onProgress?.(done, targets.length, target);
	}
	return results;
}

function rewriteMessage(message: LooseMessage, record: MapRecord): LooseMessage | undefined {
	const compacted = record.segments.filter((segment) => segment.compacted);
	if (compacted.length === 0) return undefined;

	if (typeof message.content === "string") {
		const segment = record.segments.find((candidate) => candidate.kind === "user" && candidate.compacted);
		return segment ? { ...message, content: segment.mappedText } : undefined;
	}
	if (!Array.isArray(message.content)) return undefined;

	let changed = false;
	const content = (message.content as Array<Record<string, unknown>>).map((block) => {
		if (block.type === "thinking") {
			const segment = record.segments.find((candidate) => candidate.kind === "thinking" && candidate.compacted);
			if (segment) {
				changed = true;
				return { ...block, thinking: segment.mappedText };
			}
		} else if (block.type === "text") {
			const kind: TargetKind =
				message.role === "assistant" ? "assistant" : message.role === "user" ? "user" : "toolResult";
			const segment = record.segments.find((candidate) => candidate.kind === kind && candidate.compacted);
			if (segment) {
				changed = true;
				return { ...block, text: segment.mappedText };
			}
		}
		return block;
	});
	return changed ? { ...message, content } : undefined;
}

export class CompactionMap {
	private records = new Map<string, MapRecord>();
	/** Keys in the order of the most recently observed context. */
	private order: string[] = [];
	private turn = 0;

	reset(): void {
		this.records.clear();
		this.order = [];
		this.turn = 0;
	}

	/** Observe the outgoing context, maintaining a 1:1 mapping. */
	observe(messages: unknown[]): void {
		this.turn += 1;
		for (const record of this.records.values()) record.inContext = false;

		const nextOrder: string[] = [];
		messages.forEach((raw, index) => {
			const message = (raw ?? {}) as LooseMessage;
			const key = messageKey(message);
			let record = this.records.get(key);
			if (!record) {
				const originalText = messageToText(message);
				record = {
					key,
					index,
					role: message.role ?? "unknown",
					label: messageLabel(message),
					disposition: "keep",
					originalChars: contentChars(message),
					mappedChars: contentChars(message),
					originalText,
					mappedText: originalText,
					note: "passthrough",
					segments: extractSegments(message),
					compacted: false,
					inContext: true,
					firstTurn: this.turn,
					lastTurn: this.turn,
					seen: 1,
				};
				this.records.set(key, record);
			} else {
				record.index = index;
				record.label = messageLabel(message);
				record.inContext = true;
				record.lastTurn = this.turn;
				record.seen += 1;
			}
			nextOrder.push(key);
		});

		this.order = nextOrder;
	}

	turnNumber(): number {
		return this.turn;
	}

	/** Records currently in context, in context order. */
	currentRecords(): MapRecord[] {
		return this.order
			.map((key) => this.records.get(key))
			.filter((record): record is MapRecord => record !== undefined);
	}

	/** Current records first, then detached history (most recent first). */
	allRecords(): MapRecord[] {
		const current = this.currentRecords();
		const seen = new Set(current.map((record) => record.key));
		const detached = [...this.records.values()]
			.filter((record) => !seen.has(record.key))
			.sort((a, b) => b.lastTurn - a.lastTurn);
		return [...current, ...detached];
	}

	stats(): {
		turn: number;
		inContext: number;
		detached: number;
		beforeChars: number;
		afterChars: number;
		keep: number;
		trim: number;
		summarize: number;
		drop: number;
	} {
		const result = {
			turn: this.turn,
			inContext: 0,
			detached: 0,
			beforeChars: 0,
			afterChars: 0,
			keep: 0,
			trim: 0,
			summarize: 0,
			drop: 0,
		};
		for (const record of this.records.values()) {
			if (!record.inContext) {
				result.detached += 1;
				continue;
			}
			result.inContext += 1;
			result.beforeChars += record.originalChars;
			result.afterChars += record.mappedChars;
			result[record.disposition] += 1;
		}
		return result;
	}

	/** Cheap change signature so the widget only refreshes when it matters. */
	signature(): string {
		const stats = this.stats();
		return [
			stats.inContext,
			stats.detached,
			stats.beforeChars,
			stats.afterChars,
			stats.keep,
			stats.trim,
			stats.summarize,
			stats.drop,
			this.compactedCount(),
		].join(":");
	}

	private recompute(record: MapRecord): void {
		let mapped = record.originalChars;
		for (const segment of record.segments) {
			if (segment.compacted) mapped -= segment.originalText.length - segment.mappedText.length;
		}
		record.mappedChars = Math.max(0, mapped);
		record.compacted = record.segments.some((segment) => segment.compacted);
		record.disposition = record.compacted ? "summarize" : "keep";
		record.note = "llm";
		record.mappedText = record.segments
			.map((segment) => (segment.compacted ? segment.mappedText : segment.originalText))
			.join("\n");
	}

	/** Apply one LLM summary to its record segment. */
	applySummary(recordKey: string, kind: TargetKind, text: string): void {
		const record = this.records.get(recordKey);
		if (!record) return;
		const segment = record.segments.find((candidate) => candidate.kind === kind && !candidate.compacted);
		if (!segment) return;
		segment.mappedText = text;
		segment.compacted = true;
		this.recompute(record);
	}

	isCompacted(recordKey: string, kind: TargetKind): boolean {
		return (
			this.records.get(recordKey)?.segments.some((segment) => segment.kind === kind && segment.compacted) ?? false
		);
	}

	/** Look up a record by its stable key. */
	get(recordKey: string): MapRecord | undefined {
		return this.records.get(recordKey);
	}

	/**
	 * Drop the summaries for one record so it can be re-compacted in isolation.
	 * Returns true when the record actually had a compacted segment.
	 */
	clearRecord(recordKey: string): boolean {
		const record = this.records.get(recordKey);
		if (!record) return false;
		let any = false;
		for (const segment of record.segments) {
			if (!segment.compacted) continue;
			segment.compacted = false;
			segment.mappedText = segment.originalText;
			any = true;
		}
		if (!any) return false;
		record.compacted = false;
		record.disposition = "keep";
		record.mappedChars = record.originalChars;
		record.mappedText = record.originalText;
		record.note = "passthrough";
		return true;
	}

	/** Drop all summaries so the next run redoes them. Returns messages cleared. */
	clearCompaction(): number {
		let count = 0;
		for (const key of [...this.records.keys()]) {
			if (this.clearRecord(key)) count += 1;
		}
		return count;
	}

	compactedCount(): number {
		return this.currentRecords().filter((record) => record.compacted).length;
	}

	pendingCount(): number {
		let count = 0;
		for (const record of this.currentRecords()) {
			if (record.segments.some((segment) => !segment.compacted && estimateTokens(segment.originalText.length) >= MIN_TARGET_TOKENS)) {
				count += 1;
			}
		}
		return count;
	}

	/**
	 * Rewrite seam. Replaces compacted segments in place, preserving tool-call
	 * blocks so the assistant tool-call/result pairing is never broken. Returns
	 * the input reference unchanged when disabled or nothing was compacted.
	 */
	buildRewritten(messages: unknown[], enabled: boolean): unknown[] {
		if (!enabled) return messages;
		let changed = false;
		const out = messages.map((raw) => {
			const message = (raw ?? {}) as LooseMessage;
			const record = this.records.get(messageKey(message));
			if (!record?.compacted) return raw;
			const rewritten = rewriteMessage(message, record);
			if (!rewritten) return raw;
			changed = true;
			return rewritten;
		});
		return changed ? out : messages;
	}
}
