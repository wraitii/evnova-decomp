/**
 * compact-smart — pi extension entry point.
 *
 * Observes every outgoing AgentMessage list via the `context` event and keeps a
 * 1:1 map of records (see ./core.ts). Compaction is triggered manually with
 * `/compact-smart`; `/compact-smart on|off` toggles whether compacted messages
 * replace the originals. `/compact-smart diff` and `/compact-map` open viewers.
 *
 * Everything here is defensive: the `context` handler never throws and passes
 * the original messages through unless compaction is on and applied, so a
 * reload cannot break the active session's context pipeline.
 *
 * Ghidra note: N/A (pi extension, not game code).
 */

import { convertToLlm } from "@earendil-works/pi-coding-agent";
import type {
	ExtensionAPI,
	ExtensionCommandContext,
	ExtensionContext,
} from "@earendil-works/pi-coding-agent";
import { Key, matchesKey, truncateToWidth, wrapTextWithAnsi } from "@earendil-works/pi-tui";
import {
	CompactionMap,
	clamp,
	findCompactionTargets,
	firstLinePreview,
	formatChars,
	formatTokens,
	summarizeTargets,
	type MapRecord,
} from "./core";

const WIDGET_KEY = "compact-map";
const DETAIL_WRAP_WIDTH = 92;
const MAX_DETAIL_LINES = 200;
const VIEW_BODY_ROWS = 18;

/**
 * Global apply toggle. When false the context handler is a pure observer and
 * the original messages are always sent. `/compact-smart on|off` flips it.
 */
let smartEnabled = false;

// ---------------------------------------------------------------------------
// TUI viewer
// ---------------------------------------------------------------------------

interface ViewerRow {
	text: string;
	color: string;
	kind: "record" | "detail" | "blank";
	key?: string;
}

/** What the viewer asks the extension to do after it closes. */
interface ViewerAction {
	action: "clear" | "rerun";
	key: string;
}

interface ViewerOptions {
	records: MapRecord[];
	mode: "map" | "diff";
	applyEnabled: boolean;
	theme: {
		fg: (color: string, text: string) => string;
		bg: (color: string, text: string) => string;
		bold: (text: string) => string;
	};
	requestRender: () => void;
	close: (result: ViewerAction | null) => void;
}

class CompactMapViewer {
	private readonly rows: ViewerRow[];
	private readonly records: MapRecord[];
	private readonly mode: ViewerOptions["mode"];
	private readonly applyEnabled: boolean;
	private readonly theme: ViewerOptions["theme"];
	private readonly requestRender: () => void;
	private readonly close: (result: ViewerAction | null) => void;
	private readonly expanded = new Set<string>();
	private selected = 0;
	private scrollTop = 0;

	constructor(options: ViewerOptions) {
		this.records = options.records;
		this.mode = options.mode;
		this.applyEnabled = options.applyEnabled;
		this.theme = options.theme;
		this.requestRender = options.requestRender;
		this.close = options.close;
		this.rows = this.buildRows();
	}

	private buildRows(): ViewerRow[] {
		const rows: ViewerRow[] = [];
		this.records.forEach((record, index) => {
			const detached = record.inContext ? "" : " *";
			const state = record.compacted ? "done" : "pending";
			const saved =
				record.compacted && record.originalChars > 0
					? ` -${Math.round((1 - record.mappedChars / record.originalChars) * 100)}%`
					: "";
			const size = `${formatChars(record.originalChars)}→${formatChars(record.mappedChars)}${saved}`;
			const head = `${String(index).padStart(3, "0")}  ${record.label}${detached}  [${record.disposition}/${state}]  ${size}  ${firstLinePreview(record.originalText, 70)}`;
			rows.push({ text: head, color: "text", kind: "record", key: record.key });
			if (this.expanded.has(record.key)) {
				const body = record.compacted
					? `== original (${formatChars(record.originalChars)}) ==\n${record.originalText}\n\n== compacted (${formatChars(record.mappedChars)}) ==\n${record.mappedText}`
					: `(not compacted yet)\n\n${record.originalText}`;
				const wrapped = wrapTextWithAnsi(body || "(empty)", DETAIL_WRAP_WIDTH);
				for (const line of wrapped.slice(0, MAX_DETAIL_LINES)) {
					rows.push({ text: `     │ ${line}`, color: "dim", kind: "detail", key: record.key });
				}
				rows.push({ text: "", color: "dim", kind: "blank" });
			}
		});
		return rows;
	}

	private rebuildKeepingSelection(): void {
		const anchor = this.rows[this.selected];
		const rows = this.buildRows();
		this.rows.length = 0;
		this.rows.push(...rows);
		if (anchor?.key) {
			const index = this.rows.findIndex((row) => row.kind === "record" && row.key === anchor.key);
			if (index >= 0) this.selected = index;
		}
		this.selected = clamp(this.selected, 0, Math.max(0, this.rows.length - 1));
		this.ensureVisible();
	}

	private ensureVisible(): void {
		const maxScroll = Math.max(0, this.rows.length - VIEW_BODY_ROWS);
		if (this.selected < this.scrollTop) this.scrollTop = this.selected;
		else if (this.selected >= this.scrollTop + VIEW_BODY_ROWS) {
			this.scrollTop = this.selected - VIEW_BODY_ROWS + 1;
		}
		this.scrollTop = clamp(this.scrollTop, 0, maxScroll);
	}

	private move(delta: number): void {
		this.selected = clamp(this.selected + delta, 0, Math.max(0, this.rows.length - 1));
		this.ensureVisible();
	}

	private toggleExpanded(): void {
		const row = this.rows[this.selected];
		if (!row?.key || row.kind !== "record") return;
		if (this.expanded.has(row.key)) this.expanded.delete(row.key);
		else this.expanded.add(row.key);
		this.rebuildKeepingSelection();
	}

	private emitAction(action: ViewerAction["action"]): void {
		const key = this.rows[this.selected]?.key;
		if (!key) return;
		this.close({ action, key });
	}

	handleInput(data: string): void {
		if (matchesKey(data, Key.escape) || data === "q") {
			this.close(null);
			return;
		}
		if (data === "c") {
			this.emitAction("clear");
			return;
		}
		if (data === "r") {
			this.emitAction("rerun");
			return;
		}
		if (matchesKey(data, Key.up) || data === "k") this.move(-1);
		else if (matchesKey(data, Key.down) || data === "j") this.move(1);
		else if (matchesKey(data, Key.pageUp)) this.move(-VIEW_BODY_ROWS);
		else if (matchesKey(data, Key.pageDown)) this.move(VIEW_BODY_ROWS);
		else if (matchesKey(data, Key.home)) this.move(-this.rows.length);
		else if (matchesKey(data, Key.end)) this.move(this.rows.length);
		else if (matchesKey(data, Key.enter) || matchesKey(data, Key.space)) this.toggleExpanded();
		else return;
		this.requestRender();
	}

	handleMouse(event: { type: string; wheelDelta?: number }): { handled: boolean } | undefined {
		if (event.type !== "wheel") return undefined;
		this.move(event.wheelDelta ?? 0);
		this.requestRender();
		return { handled: true };
	}

	invalidate(): void {
		// Rows are plain text; colors are applied fresh on every render.
	}

	render(width: number): string[] {
		const w = Math.max(20, width);
		const divider = truncateToWidth("─".repeat(w), w);

		const stats = {
			before: this.records.reduce((sum, record) => sum + (record.inContext ? record.originalChars : 0), 0),
			after: this.records.reduce((sum, record) => sum + (record.inContext ? record.mappedChars : 0), 0),
			inContext: this.records.filter((record) => record.inContext).length,
		};

		const compacted = this.records.filter((record) => record.inContext && record.compacted).length;
		const saved = stats.before > 0 ? Math.round((1 - stats.after / stats.before) * 100) : 0;
		const title = this.mode === "diff" ? "Compaction diff" : "Compaction map";
		const out: string[] = [];
		out.push(
			this.theme.fg("accent", this.theme.bold(`${title} · ${this.applyEnabled ? "APPLY ON" : "observe only"}`)),
		);
		out.push(
			this.theme.fg(
				"muted",
				`${stats.inContext} in context · ${compacted} compacted · ${formatChars(stats.before)} → ${formatChars(stats.after)} (-${saved}%) · ${this.expanded.size} expanded`,
			),
		);
		out.push(this.theme.fg("dim", divider));

		for (let row = 0; row < VIEW_BODY_ROWS; row += 1) {
			const index = this.scrollTop + row;
			const item = this.rows[index];
			if (!item) {
				out.push("");
				continue;
			}
			const text = truncateToWidth(item.text, w, "…", true);
			if (index === this.selected) out.push(this.theme.bg("selectedBg", this.theme.fg(item.color, text)));
			else out.push(this.theme.fg(item.color, text));
		}

		out.push(this.theme.fg("dim", divider));
		out.push(
			this.theme.fg(
				"muted",
				truncateToWidth(
					`↑↓ move · enter expand · c clear · r rerun · PgUp/PgDn · home/end · wheel · q close    ${this.selected + 1}/${this.rows.length}`,
					w,
				),
			),
		);
		return out;
	}
}

// ---------------------------------------------------------------------------
// Extension wiring
// ---------------------------------------------------------------------------

const map = new CompactionMap();
let lastWidgetSignature = "";

function updateWidget(ctx: ExtensionContext, force = false): void {
	if (!ctx.hasUI) return;
	const signature = `${smartEnabled ? 1 : 0}:${map.signature()}`;
	if (!force && signature === lastWidgetSignature) return;
	lastWidgetSignature = signature;

	const stats = map.stats();
	const saved = stats.beforeChars > 0 ? Math.round((1 - stats.afterChars / stats.beforeChars) * 100) : 0;
	ctx.ui.setWidget(WIDGET_KEY, [
		`compact-smart [${smartEnabled ? "ON" : "off"}] · ${stats.inContext} msgs · ${formatChars(stats.beforeChars)} → ${formatChars(stats.afterChars)} (-${saved}%) · ~${formatTokens(stats.beforeChars)} tok`,
		`turn ${stats.turn} · compacted ${map.compactedCount()} · pending ${map.pendingCount()} · /compact-smart run|rerun|clear|on|off|diff`,
	]);
}

export default function (pi: ExtensionAPI) {
	pi.on("session_start", async (_event, ctx) => {
		try {
			map.reset();
			lastWidgetSignature = "";
			updateWidget(ctx, true);
		} catch (error) {
			console.error("[compact-smart] session_start failed", error);
		}
	});

	pi.on("session_shutdown", async (_event, ctx) => {
		try {
			if (ctx.hasUI) ctx.ui.setWidget(WIDGET_KEY, undefined);
			lastWidgetSignature = "";
			map.reset();
		} catch (error) {
			console.error("[compact-smart] session_shutdown failed", error);
		}
	});

	// Observe the outgoing context and apply cached compaction when enabled.
	pi.on("context", async (event, ctx) => {
		try {
			map.observe(event.messages as unknown[]);
			updateWidget(ctx);
			const rewritten = map.buildRewritten(event.messages as unknown[], smartEnabled);
			if (rewritten !== (event.messages as unknown[])) {
				return { messages: rewritten as typeof event.messages };
			}
		} catch (error) {
			console.error("[compact-smart] context observation failed", error);
		}
		return undefined;
	});

	// Report the mapping at compaction time without replacing the default
	// compaction behavior.
	pi.on("session_before_compact", async (_event, ctx) => {
		try {
			const stats = map.stats();
			ctx.ui.notify(
				`compact-smart: ${stats.inContext} mapped msgs (~${formatTokens(stats.beforeChars)} tok); apply is ${smartEnabled ? "ON" : "off"}`,
				"info",
			);
		} catch (error) {
			console.error("[compact-smart] session_before_compact reporting failed", error);
		}
		return undefined;
	});

	pi.registerCommand("compact-map", {
		description: "Inspect the compaction message map",
		handler: async (_args, ctx) => openViewer(ctx, "map"),
	});

	pi.registerCommand("compact-smart", {
		description: "Manual compaction: run (default) | rerun | clear | on | off | diff | map | status",
		handler: async (args, ctx) => {
			const arg = (args ?? "").trim().toLowerCase();
			try {
				if (arg === "on" || arg === "off") {
					smartEnabled = arg === "on";
					lastWidgetSignature = "";
					updateWidget(ctx, true);
					ctx.ui.notify(
						smartEnabled
							? "compact-smart: ON — compacted messages will be sent"
							: "compact-smart: off — original messages only",
						"info",
					);
					return;
				}
				if (arg === "diff") return openViewer(ctx, "diff");
				if (arg === "map") return openViewer(ctx, "map");
				if (arg === "status") {
					const stats = map.stats();
					ctx.ui.notify(
						`compact-smart [${smartEnabled ? "ON" : "off"}] · ${stats.inContext} msgs · compacted ${map.compactedCount()} · pending ${map.pendingCount()} · ${formatChars(stats.beforeChars)} → ${formatChars(stats.afterChars)}`,
						"info",
					);
					return;
				}
				if (arg === "clear") {
					const count = map.clearCompaction();
					lastWidgetSignature = "";
					updateWidget(ctx, true);
					ctx.ui.notify(`compact-smart: cleared ${count} compacted messages — /compact-smart to redo`, "info");
					return;
				}
				if (arg === "rerun") {
					map.clearCompaction();
					await runLlmCompaction(ctx);
					return;
				}
				if (arg === "" || arg === "run") {
					await runLlmCompaction(ctx);
					return;
				}
				ctx.ui.notify("compact-smart: usage — /compact-smart [run|rerun|clear|on|off|diff|map|status]", "warning");
			} catch (error) {
				const message = error instanceof Error ? error.message : String(error);
				ctx.ui.notify(`compact-smart failed: ${message}`, "error");
			}
		},
	});
}

async function runLlmCompaction(ctx: ExtensionCommandContext, recordKey?: string): Promise<void> {
	const model = ctx.model;
	if (!model) {
		ctx.ui.notify("compact-smart: no active model", "error");
		return;
	}
	if (!ctx.modelRegistry.hasConfiguredAuth(model)) {
		ctx.ui.notify(`compact-smart: no auth configured for ${model.provider}/${model.id}`, "error");
		return;
	}

	const messages = ctx.sessionManager.buildSessionContext().messages as unknown[];
	map.observe(messages);
	const targets = findCompactionTargets(messages).filter(
		(target) => (!recordKey || target.recordKey === recordKey) && !map.isCompacted(target.recordKey, target.kind),
	);
	if (targets.length === 0) {
		ctx.ui.notify("compact-smart: nothing to compact (no uncached segments above threshold)", "info");
		return;
	}

	const sessionId = ctx.sessionManager.getSessionId();
	let failed = 0;
	ctx.ui.notify(
		`compact-smart: summarizing ${targets.length} segments with ${model.provider}/${model.id}...`,
		"info",
	);

	const results = await summarizeTargets(
		messages,
		targets,
		(prefix) => convertToLlm(prefix as never) as unknown[],
		async (request) => {
			try {
				const response = await ctx.modelRegistry.complete(
					model,
					{ messages: request as never },
					// Low reasoning, near-deterministic sampling. reasoningEffort covers
					// OpenAI-compatible providers (OpenRouter); Anthropic would need effort/thinking.
					{ temperature: 0.1, sessionId, reasoningEffort: "low" },
				);
				return response.content
					.map((block) => (block.type === "text" ? block.text : ""))
					.filter((text) => text.length > 0)
					.join("\n");
			} catch (error) {
				failed += 1;
				const message = error instanceof Error ? error.message : String(error);
				ctx.ui.notify(`compact-smart: ${message}`, "warning");
				return "";
			}
		},
		(done, total, target) => {
			ctx.ui.setStatus("compact-smart", `compacting ${done}/${total} · ${target.label}`);
			updateWidget(ctx);
		},
	);

	ctx.ui.setStatus("compact-smart", undefined);
	for (const result of results) map.applySummary(result.recordKey, result.kind, result.summary);
	lastWidgetSignature = "";
	updateWidget(ctx, true);

	const before = results.reduce((sum, result) => sum + result.originalChars, 0);
	const after = results.reduce((sum, result) => sum + result.summary.length, 0);
	const saved = before > 0 ? Math.round((1 - after / before) * 100) : 0;
	ctx.ui.notify(
		`compact-smart: summarized ${results.length}/${targets.length} segments${failed ? ` (${failed} failed)` : ""} · ${formatChars(before)} → ${formatChars(after)} (-${saved}%) · /compact-smart diff to inspect, /compact-smart on to apply`,
		"info",
	);
}

async function openViewer(ctx: ExtensionCommandContext, mode: "map" | "diff"): Promise<void> {
	if (ctx.mode !== "tui") {
		ctx.ui.notify(`compact-${mode}: interactive TUI only`, "warning");
		return;
	}
	// The viewer closes for any action so compaction can run outside the
	// overlay; reopen it afterwards so the updated state is visible.
	for (;;) {
		const records = map.allRecords();
		if (records.length === 0) {
			ctx.ui.notify("compact: no messages observed yet — send a prompt first", "info");
			return;
		}
		let action: ViewerAction | null = null;
		try {
			action = await ctx.ui.custom<ViewerAction | null>(
				(tui, theme, _keybindings, done) => {
					const viewer = new CompactMapViewer({
						records,
						mode,
						applyEnabled: smartEnabled,
						theme,
						requestRender: () => tui.requestRender(),
						close: (result) => done(result),
					});
					return {
						render: (width: number) => viewer.render(width),
						handleInput: (data: string) => viewer.handleInput(data),
						handleMouse: (event: { type: string; wheelDelta?: number }) => viewer.handleMouse(event),
						invalidate: () => viewer.invalidate(),
					};
				},
				{
					overlay: true,
					overlayOptions: { width: "92%", maxHeight: "85%", anchor: "center", margin: 1 },
				},
			);
		} catch (error) {
			const message = error instanceof Error ? error.message : String(error);
			ctx.ui.notify(`compact-${mode} failed: ${message}`, "error");
			return;
		}
		if (!action) return;

		const record = map.get(action.key);
		if (!record) continue;

		if (action.action === "clear") {
			const changed = map.clearRecord(action.key);
			lastWidgetSignature = "";
			updateWidget(ctx, true);
			ctx.ui.notify(
				changed
					? `compact-smart: cleared ${record.label} — press r to redo`
					: `compact-smart: ${record.label} was not compacted`,
				"info",
			);
		} else {
			// Rerun: drop this message's summaries, then redo only this message.
			map.clearRecord(action.key);
			await runLlmCompaction(ctx, action.key);
		}
	}
}
