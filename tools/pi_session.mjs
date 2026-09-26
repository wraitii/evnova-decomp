#!/usr/bin/env node
// File-protocol supervisor around `pi --mode rpc`.
//
// A non-Pi caller (Codex, Claude Code, a shell script) drives one Pi session:
// it drops request files into <run>/inbox and reads authoritative state from
// <run>/status.json. Every request is acknowledged in status.lastRequest so a
// caller that never sees this process's stdout can still confirm delivery.
//
// Commands: start | status | wait | talk | prompt | steer | abort | close
//   start   run the supervisor (foreground; background it from the caller)
//   status  print status.json, exit 5 if the launcher is gone
//   wait    block until the session settles, then report (--timeout SECONDS)
//   talk    prompt when idle, steer when running - the usual "say something"
//   prompt  start a turn (refused unless idle)
//   steer   inject into the running turn (refused unless running)
//   abort   stop the current turn, return to idle
//   close   end the session (refused unless idle)
//
// No dependencies beyond Node and Pi.
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs';
import * as path from 'node:path';

const BOOLEAN_FLAGS = new Set(['json']);
const [action, ...args] = process.argv.slice(2);
const options = {};
for (let i = 0; i < args.length; i++) {
  const token = args[i];
  if (!token.startsWith('--')) throw new Error(`Unexpected argument: ${token}`);
  const eq = token.indexOf('=');
  if (eq !== -1) options[token.slice(2, eq)] = token.slice(eq + 1);
  else if (BOOLEAN_FLAGS.has(token.slice(2))) options[token.slice(2)] = true;
  else {
    const value = args[++i];
    if (value === undefined) throw new Error(`Missing value for ${token}`);
    options[token.slice(2)] = value;
  }
}

const USAGE =
  'Usage: pi_session.mjs start|status|wait|talk|prompt|steer|abort|close --run DIR ' +
  '[--task FILE | --message TEXT] [--budget 0.50] [--timeout SECONDS] [--json]';

const dir = options.run ? path.resolve(options.run) : process.cwd();
const statusPath = path.join(dir, 'status.json');
const eventsPath = path.join(dir, 'events.jsonl');
const inbox = path.join(dir, 'inbox');
const PROVIDER = options.provider ?? 'openrouter';
const MODEL = options.model ?? 'deepseek/deepseek-v4.1-flash';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const log = message => process.stderr.write(message + '\n');

function emit(payload, lines = [], code = 0) {
  const body = options.json || lines.length === 0 ? JSON.stringify(payload) : lines.join('\n');
  process.stdout.write(body + '\n');
  return code;
}
function fail(message, code = 1) {
  return emit({ ok: false, error: message }, [`Error: ${message}`], code);
}
function readStatus() {
  try {
    return JSON.parse(fs.readFileSync(statusPath, 'utf8'));
  } catch {
    return null;
  }
}
function pidAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    return err.code === 'EPERM';
  }
}
function isRunningState(state) {
  return state === 'starting' || state === 'running';
}
function isFinalState(state) {
  return state === 'idle' || state === 'budget-limit' || state === 'failed' || state === 'closed';
}

function readMessage() {
  if (options.message !== undefined) return options.message;
  if (options.task === '-') return fs.readFileSync(0, 'utf8');
  if (options.task) return fs.readFileSync(path.resolve(options.task), 'utf8');
  throw new Error('--task FILE or --message TEXT is required');
}

// ---------------------------------------------------------------------------
// Client commands
// ---------------------------------------------------------------------------

function statusAction() {
  const status = readStatus();
  if (!status) return (process.exitCode = fail(`No status file at ${statusPath}`));
  const stale = isRunningState(status.state) && status.launcherPid && !pidAlive(status.launcherPid);
  const last = status.lastRequest;
  const lines = [
    `state: ${status.state}${stale ? ' (launcher not running)' : ''}`,
    `model: ${status.model ?? '?'}  cost: $${status.cost ?? '?'}  context: ${status.contextPercent ?? '?'}%`,
    `tools: ${status.toolCalls}  last tool: ${status.lastTool ?? '-'}  result: ${status.result ?? '-'}`,
    last ? `last request: ${last.action} ${last.accepted ? 'accepted' : 'refused'}${last.error ? ` (${last.error})` : ''}` : null,
  ].filter(Boolean);
  process.exitCode = emit({ ...status, stale: Boolean(stale) }, lines, stale ? 5 : 0);
}

async function waitAction() {
  const timeoutMs = options.timeout ? Number(options.timeout) * 1000 : 0;
  const deadline = timeoutMs > 0 ? Date.now() + timeoutMs : Infinity;
  for (;;) {
    const status = readStatus();
    if (!status) return (process.exitCode = fail(`No status file at ${statusPath}`));
    if (isFinalState(status.state)) return (process.exitCode = reportFinal(status));
    if (status.launcherPid && !pidAlive(status.launcherPid)) {
      return (process.exitCode = emit(
        { ok: false, state: status.state, error: 'Launcher is no longer running', result: status.result },
        ['Error: launcher is no longer running'],
        5,
      ));
    }
    if (Date.now() >= deadline) {
      return (process.exitCode = emit(
        { ok: false, timeout: true, state: status.state, cost: status.cost, contextPercent: status.contextPercent },
        [`Timed out after ${options.timeout}s (state: ${status.state})`],
        2,
      ));
    }
    await sleep(200);
  }
}

function reportFinal(status) {
  const failed = status.state === 'failed' || status.stopReason === 'error';
  const code = failed ? 4 : status.state === 'budget-limit' ? 3 : 0;
  const payload = {
    ok: code === 0,
    state: status.state,
    result: status.result,
    stopReason: status.stopReason,
    cost: status.cost,
    contextPercent: status.contextPercent,
    error: status.error ?? null,
    settledAt: status.settledAt ?? null,
  };
  const lines = [
    `settled: ${status.state}${status.stopReason ? ` (${status.stopReason})` : ''}`,
    `cost: $${status.cost ?? '?'}  context: ${status.contextPercent ?? '?'}%`,
    `result: ${status.result ?? '(none)'}`,
  ];
  return emit(payload, lines, code);
}

async function requestAction() {
  const type = action;
  const fields = {};
  if (type === 'talk' || type === 'prompt' || type === 'steer') fields.message = readMessage();
  const id = `req-${Date.now()}-${randomUUID().slice(0, 8)}`;
  fs.mkdirSync(inbox, { recursive: true });
  const stamp = `${Date.now()}-${randomUUID()}`;
  const temp = path.join(inbox, `${stamp}.tmp`);
  fs.writeFileSync(temp, JSON.stringify({ id, type, ...fields }));
  fs.renameSync(temp, path.join(inbox, `${stamp}.json`));

  const ackTimeoutMs = options['ack-timeout'] !== undefined ? Number(options['ack-timeout']) * 1000 : 10000;
  if (ackTimeoutMs <= 0) {
    return (process.exitCode = emit({ requestId: id, queued: true }, [`Queued ${type} (${id}).`]));
  }
  const deadline = Date.now() + ackTimeoutMs;
  while (Date.now() < deadline) {
    const status = readStatus();
    if (status?.lastRequest?.id === id) {
      return (process.exitCode = emit(
        { requestId: id, accepted: status.lastRequest.accepted, error: status.lastRequest.error ?? null, state: status.state },
        [`${type}: ${status.lastRequest.accepted ? 'accepted' : 'refused'}${status.lastRequest.error ? ` (${status.lastRequest.error})` : ''}`],
        status.lastRequest.accepted ? 0 : 1,
      ));
    }
    await sleep(150);
  }
  return (process.exitCode = emit(
    { requestId: id, accepted: null, error: 'Timed out waiting for acknowledgement' },
    ['Error: no acknowledgement from the supervisor'],
    2,
  ));
}

// ---------------------------------------------------------------------------
// Supervisor
// ---------------------------------------------------------------------------

async function startAction() {
  const budget = Number(options.budget ?? 0.5);
  if (!(budget > 0 && Number.isFinite(budget))) throw new Error('Budget must be positive');
  const task = readMessage();
  const pollMs = Number(options['poll-ms'] ?? 500);
  if (!(pollMs > 0)) throw new Error('--poll-ms must be positive');

  fs.mkdirSync(inbox, { recursive: true });
  // A run directory is single-use: session cost and queued commands must not
  // leak between launches. Use talk/prompt on an idle process to continue.
  const startedPath = path.join(dir, 'started.json');
  if (fs.existsSync(startedPath)) throw new Error(`Run directory is already in use: ${dir}`);
  fs.writeFileSync(startedPath, JSON.stringify({ cwd: process.cwd(), budget, pid: process.pid }), { flag: 'wx' });

  const status = {
    state: 'starting',
    budget,
    cost: null,
    contextPercent: null,
    contextWindow: null,
    tokens: null,
    model: null,
    sessionFile: null,
    toolCalls: 0,
    lastTool: null,
    result: null,
    stopReason: null,
    error: null,
    startedAt: Date.now(),
    updatedAt: Date.now(),
    lastEventAt: null,
    settledAt: null,
    lastRequest: null,
    launcherPid: process.pid,
  };
  const save = () => {
    status.updatedAt = Date.now();
    fs.writeFileSync(statusPath + '.tmp', JSON.stringify(status, null, 2));
    fs.renameSync(statusPath + '.tmp', statusPath);
  };
  save();

  const child = spawn(
    options.pi ?? 'pi',
    ['--provider', PROVIDER, '--model', MODEL, '--mode', 'rpc', '--session-dir', path.join(dir, 'sessions'),
      '--no-extensions', '--no-skills', '--no-prompt-templates'],
    { stdio: ['pipe', 'pipe', 'pipe'] },
  );

  let buffer = '';
  let sequence = 0;
  let ready = false;
  let closing = false;
  let closed = false;
  let limited = false;
  let buildRunning = false;
  let activeRun = false;
  let windingDown = false;
  let budgetWarning = false;
  let stopping = false;
  let turns = 0;
  let lastPoll = 0;
  const pending = new Map();

  function send(type, fields = {}) {
    const id = `c${++sequence}`;
    if (!child.stdin.writable) throw new Error('Pi stdin is closed');
    child.stdin.write(JSON.stringify({ id, type, ...fields }) + '\n');
    return id;
  }
  function call(type, fields = {}, timeoutMs = 15000) {
    return new Promise((resolve, reject) => {
      const id = send(type, fields);
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(new Error(`${type} timed out`));
      }, timeoutMs);
      pending.set(id, response => {
        clearTimeout(timer);
        resolve(response);
      });
    });
  }
  function ack(id, kind, accepted, error = null) {
    status.lastRequest = { id, action: kind, accepted, error, at: Date.now() };
    save();
  }
  async function stop() {
    if (stopping) return;
    stopping = true;
    try {
      await call('clear_queue', {}, 10000).catch(() => {});
      await call('abort', {}, 300000).catch(() => {});
      if (!activeRun && status.state !== 'budget-limit') {
        status.state = 'idle';
        save();
      }
    } finally {
      stopping = false;
    }
  }

  async function pollStats() {
    if (closing) return;
    let response;
    try {
      response = await call('get_session_stats', {}, 15000);
    } catch {
      return;
    }
    if (response.success === false) return;
    const data = response.data ?? {};
    if (typeof data.cost === 'number') status.cost = data.cost;
    if (data.tokens) status.tokens = data.tokens;
    if (typeof data.contextUsage?.percent === 'number') status.contextPercent = data.contextUsage.percent;
    if (data.sessionFile) status.sessionFile = data.sessionFile;
    save();

    if (!windingDown && typeof status.contextPercent === 'number' && status.contextPercent >= 20) {
      windingDown = true;
      call('steer', { message: 'This session is getting long. Finish the current changes and validation, save the continuation note, and wrap up. Leave new investigation for a fresh session.' }).catch(() => {});
      log('Sent context wind-down reminder.');
    }
    if (!budgetWarning && typeof status.cost === 'number' && status.cost >= budget * 0.75) {
      budgetWarning = true;
      call('steer', { message: 'We are approaching the session budget. Finish the current work and save the result and remaining checks.' }).catch(() => {});
      log('Sent budget reminder.');
    }
    if (!limited && typeof status.cost === 'number' && status.cost >= budget) {
      limited = true;
      status.state = 'budget-limit';
      save();
      log('Budget reached; stopping inference after any running build.');
      if (!buildRunning) await stop();
    }
  }

  function handleEvent(event) {
    if (event.type === 'response') {
      const resolve = pending.get(event.id);
      if (resolve) {
        pending.delete(event.id);
        resolve(event);
      }
      return;
    }
    status.lastEventAt = Date.now();
    if (event.type === 'tool_execution_start') {
      status.toolCalls += 1;
      status.lastTool = event.toolName;
      buildRunning = event.toolName === 'bash' && /\b(cmake\s+--build|ctest|ninja)\b/.test(event.args?.command ?? '');
      save();
    }
    if (event.type === 'tool_execution_end') {
      buildRunning = false;
      save();
      if (limited) stop();
    }
    if (event.type === 'message_end' && event.message?.role === 'assistant') {
      if (['stop', 'error', 'aborted', 'length'].includes(event.message.stopReason)) {
        const content = Array.isArray(event.message.content)
          ? event.message.content.filter(part => part?.type === 'text').map(part => part.text).join('\n')
          : String(event.message.content ?? '');
        const result = path.join(dir, `result-${++turns}.md`);
        fs.writeFileSync(result, content);
        status.result = result;
        status.stopReason = event.message.stopReason;
        save();
      }
    }
    if (event.type === 'agent_settled') {
      activeRun = false;
      status.settledAt = Date.now();
      status.state = limited ? 'budget-limit' : 'idle';
      save();
      pollStats().catch(() => {});
      log(`Pi is settled (${status.state}). ${status.result ?? 'no result yet'}`);
    }
  }

  async function handleRequest(request) {
    if (!request?.id || !request.type) throw new Error('Malformed request');
    switch (request.type) {
      case 'talk':
      case 'prompt': {
        if (!ready) throw new Error('Session is not ready yet');
        if (closing) throw new Error('Session is closing');
        if (request.type === 'prompt' && status.state !== 'idle') throw new Error(`Session is ${status.state}; use talk or steer`);
        if (status.state === 'idle') {
          if (limited) throw new Error('Session budget is exhausted');
          if (typeof status.contextPercent === 'number' && status.contextPercent >= 30) throw new Error('Session context is too full for a new turn');
          if (typeof status.cost === 'number' && status.cost >= budget) throw new Error('Session budget is exhausted');
          status.state = 'running';
          status.stopReason = null;
          activeRun = true;
          save();
          const response = await call('prompt', { message: request.message }, 30000);
          if (response.success === false) {
            status.state = 'idle';
            activeRun = false;
            save();
            throw new Error(response.error ?? 'prompt rejected');
          }
          ack(request.id, request.type, true);
        } else if (request.type === 'talk' && status.state === 'running') {
          const response = await call('steer', { message: request.message });
          if (response.success === false) throw new Error(response.error ?? 'steer rejected');
          ack(request.id, 'talk', true);
        } else {
          throw new Error(`Cannot ${request.type} while ${status.state}`);
        }
        break;
      }
      case 'steer': {
        if (!ready) throw new Error('Session is not ready yet');
        if (status.state !== 'running') throw new Error(`Session is ${status.state}; use talk`);
        const response = await call('steer', { message: request.message });
        if (response.success === false) throw new Error(response.error ?? 'steer rejected');
        ack(request.id, 'steer', true);
        break;
      }
      case 'abort': {
        ack(request.id, 'abort', true);
        await stop();
        break;
      }
      case 'close': {
        if (status.state !== 'idle') throw new Error(`Session is ${status.state}; wait or abort first`);
        closing = true;
        ack(request.id, 'close', true);
        child.stdin.end();
        setTimeout(() => child.kill('SIGTERM'), 1500).unref();
        break;
      }
      default:
        throw new Error(`Unknown request type: ${request.type}`);
    }
  }

  async function processInbox() {
    if (!fs.existsSync(inbox)) return;
    for (const name of fs.readdirSync(inbox).filter(entry => entry.endsWith('.json')).sort()) {
      const file = path.join(inbox, name);
      let request;
      try {
        request = JSON.parse(fs.readFileSync(file, 'utf8'));
      } catch (err) {
        fs.renameSync(file, file + '.failed');
        log(`Ignored malformed request ${name}: ${err.message}`);
        continue;
      }
      fs.renameSync(file, file + '.processed');
      try {
        await handleRequest(request);
      } catch (err) {
        ack(request.id, request.type, false, err.message);
        log(`Refused ${request.type}: ${err.message}`);
      }
    }
  }

  child.stderr.on('data', data => fs.appendFileSync(path.join(dir, 'stderr.log'), data));
  child.stdout.on('data', data => {
    fs.appendFileSync(eventsPath, data);
    buffer += data;
    for (;;) {
      const end = buffer.indexOf('\n');
      if (end < 0) break;
      const line = buffer.slice(0, end);
      buffer = buffer.slice(end + 1);
      let event;
      try {
        event = JSON.parse(line);
      } catch {
        continue;
      }
      try {
        handleEvent(event);
      } catch (err) {
        log(`Event handling error: ${err.message}`);
      }
    }
  });

  async function settleStartup() {
    const response = await call('get_state', {}, 30000);
    if (response.success === false) throw new Error(response.error ?? 'get_state failed');
    const model = response.data?.model;
    if (model?.provider !== PROVIDER || model?.id !== MODEL) {
      throw new Error(`Unexpected model: ${model?.provider}/${model?.id}`);
    }
    ready = true;
    status.model = `${model.provider}/${model.id}`;
    status.contextWindow = model.contextWindow ?? null;
    status.error = null;
    status.state = 'running';
    activeRun = true;
    save();
    log(`Pi started with ${status.model}.`);
    const response2 = await call('prompt', { message: task }, 30000);
    if (response2.success === false) throw new Error(response2.error ?? 'initial prompt rejected');
  }

  settleStartup().catch(err => {
    status.error = err.message;
    status.state = 'failed';
    save();
    log(`Startup failed: ${err.message}`);
    child.kill('SIGTERM');
  });

  let ticking = false;
  const ticker = setInterval(async () => {
    if (ticking || closed) return;
    ticking = true;
    try {
      await processInbox();
      if (ready && !closing && Date.now() - lastPoll >= 30000) {
        lastPoll = Date.now();
        pollStats().catch(() => {});
      }
    } catch (err) {
      log(`Inbox error: ${err.message}`);
    } finally {
      ticking = false;
    }
  }, pollMs);
  const heartbeat = setInterval(() => {
    if (closing) return;
    log(`Pi ${status.state}: ${status.toolCalls} tools, $${status.cost ?? '?'}, context ${status.contextPercent?.toFixed(1) ?? '?'}%.`);
  }, 60000);

  child.on('error', err => {
    status.error = err.message;
    status.state = 'failed';
    save();
    clearInterval(ticker);
    clearInterval(heartbeat);
  });
  child.on('exit', (code, signal) => {
    closed = true;
    if (status.state !== 'failed') status.state = 'closed';
    status.exitCode = code;
    status.signal = signal;
    save();
    clearInterval(ticker);
    clearInterval(heartbeat);
    log('Pi process closed.');
  });
}

const KNOWN = ['start', 'status', 'wait', 'talk', 'prompt', 'steer', 'abort', 'close'];
try {
  if (!options.run) throw new Error(USAGE);
  if (!KNOWN.includes(action)) {
    process.exitCode = fail(`Unknown action: ${action}\n${USAGE}`);
  } else if (action === 'status') {
    statusAction();
  } else if (action === 'wait') {
    await waitAction();
  } else if (action !== 'start') {
    await requestAction();
  } else {
    await startAction();
  }
} catch (err) {
  process.exitCode = fail(err?.message ?? String(err));
}
