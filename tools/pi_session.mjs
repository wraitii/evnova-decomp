#!/usr/bin/env node
// Small RPC supervisor: keep detailed Pi output on disk, and an idle session
// available for review/follow-ups. No dependencies beyond Node and Pi.
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import * as fs from 'node:fs';
import path from 'node:path';

const [action, ...args] = process.argv.slice(2);
const options = {};
for (let i = 0; i < args.length; i += 2) {
  if (!args[i].startsWith('--') || args[i + 1] === undefined) {
    throw new Error('Expected --option value pairs');
  }
  options[args[i].slice(2)] = args[i + 1];
}
if (!options.run) {
  throw new Error('Usage: pi_session.mjs start|status|prompt|steer|close|abort --run DIR [--task FILE] [--budget 0.50]');
}
const dir = path.resolve(options.run);
const statusPath = path.join(dir, 'status.json');
const inbox = path.join(dir, 'inbox');
if (action === 'status') {
  process.stdout.write(fs.readFileSync(statusPath, 'utf8') + '\n');
  process.exit(0);
}
if (action !== 'start') {
  if (!['prompt', 'steer', 'close', 'abort'].includes(action)) throw new Error('Unknown action');
  const request = { type: action };
  if (action === 'prompt' || action === 'steer') {
    if (!options.task) throw new Error('--task FILE is required');
    request.message = fs.readFileSync(options.task, 'utf8');
  }
  const name = `${Date.now()}-${randomUUID()}`;
  const temp = path.join(inbox, name + '.tmp');
  fs.writeFileSync(temp, JSON.stringify(request));
  fs.renameSync(temp, path.join(inbox, name + '.json'));
  console.log('Queued ' + action + '; check status for acknowledgement.');
  process.exit(0);
}
if (!options.task) throw new Error('--task FILE is required');
const budget = Number(options.budget ?? 0.50);
if (!(budget > 0 && Number.isFinite(budget))) throw new Error('Budget must be positive');
const task = fs.readFileSync(options.task, 'utf8');
fs.mkdirSync(inbox, { recursive: true });
// Require a new run directory: session costs and queued commands must not leak
// between launches. Use prompt to continue an existing idle process instead.
fs.writeFileSync(path.join(dir, 'started.json'), JSON.stringify({ cwd: process.cwd(), budget }), { flag: 'wx' });
let status = { state: 'starting', budget, cost: null, contextPercent: null, toolCalls: 0, lastTool: null, result: null };
const save = () => {
  fs.writeFileSync(statusPath + '.tmp', JSON.stringify(status, null, 2));
  fs.renameSync(statusPath + '.tmp', statusPath);
};
const announce = message => console.log(message);
save();
const child = spawn(options.pi ?? 'pi', ['--provider', 'openrouter', '--model', 'deepseek/deepseek-v4.1-flash',
  '--mode', 'rpc', '--session-dir', path.join(dir, 'sessions'), '--no-extensions', '--no-skills', '--no-prompt-templates'],
{ stdio: ['pipe', 'pipe', 'pipe'] });
let buffer = '', sequence = 0, ready = false, windingDown = false, budgetWarning = false;
let limited = false, buildRunning = false, closing = false, turns = 0;
let lastPoll = 0;
const send = (type, fields = {}) => child.stdin.write(JSON.stringify({ id: `control-${++sequence}`, type, ...fields }) + '\n');
const stop = () => send('clear_queue'); // abort after acknowledgement
child.stderr.on('data', data => fs.appendFileSync(path.join(dir, 'stderr.log'), data));
child.stdout.on('data', data => {
  fs.appendFileSync(path.join(dir, 'events.jsonl'), data);
  buffer += data;
  for (;;) {
    const end = buffer.indexOf('\n');
    if (end < 0) break;
    const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
    let e; try { e = JSON.parse(line); } catch { continue; }
    if (e.type === 'response') {
      if (!e.success) { status.error = e.error; save(); announce(`RPC ${e.command} failed; see status/log.`); continue; }
      if (e.command === 'get_state' && !ready) {
        const m = e.data?.model;
        if (m?.provider !== 'openrouter' || m?.id !== 'deepseek/deepseek-v4.1-flash') {
          status.error = 'Unexpected model'; child.kill('SIGTERM'); continue;
        }
        ready = true; status.model = `${m.provider}/${m.id}`; status.contextWindow = m.contextWindow;
        status.state = 'running'; save(); send('prompt', { message: task }); announce('Pi started with requested model.');
      }
      if (e.command === 'get_session_stats') {
        status.cost = e.data.cost ?? null; status.contextPercent = e.data.contextUsage?.percent ?? null;
        status.tokens = e.data.tokens; status.sessionFile = e.data.sessionFile; save();
        if (!windingDown && status.contextPercent >= 20) {
          windingDown = true;
          send('steer', { message: 'This session is getting long. Finish the current changes and validation, save the continuation note, and wrap up. Leave new investigation for a fresh session.' });
          announce('Sent context wind-down reminder.');
        }
        if (!budgetWarning && status.cost >= budget * 0.75) {
          budgetWarning = true; send('steer', { message: 'We are approaching the session budget. Finish the current work and save the result and remaining checks.' });
          announce('Sent budget reminder.');
        }
        if (!limited && status.cost >= budget) {
          limited = true; status.state = 'budget-limit'; save();
          announce('Budget reached; stopping inference after any running build.');
          if (!buildRunning) stop();
        }
      }
      if (e.command === 'clear_queue') send('abort');
      if (e.command === 'abort') { status.state = 'idle'; save(); send('get_session_stats'); }
    }
    if (e.type === 'tool_execution_start') {
      status.toolCalls++; status.lastTool = e.toolName;
      buildRunning = e.toolName === 'bash' && /\b(cmake\s+--build|ctest|ninja)\b/.test(e.args?.command ?? ''); save();
    }
    if (e.type === 'tool_execution_end') { buildRunning = false; if (limited) stop(); }
    if (e.type === 'message_end' && e.message?.role === 'assistant') {
      if (['stop', 'error', 'aborted', 'length'].includes(e.message.stopReason)) {
        const content = e.message.content.filter(c => c.type === 'text').map(c => c.text).join('\n');
        const result = path.join(dir, `result-${++turns}.md`); fs.writeFileSync(result, content);
        status.result = result; status.stopReason = e.message.stopReason; save();
      }
    }
    if (e.type === 'agent_end') {
      status.state = 'idle'; save(); send('get_session_stats');
      announce(`Pi is idle. Review ${status.result ?? 'events.jsonl'}; session remains open for follow-up.`);
    }
  }
});
const timer = setInterval(() => {
  if (ready && !closing && Date.now() - lastPoll >= 30000) {
    lastPoll = Date.now(); send('get_session_stats');
  }
  for (const name of fs.readdirSync(inbox).filter(n => n.endsWith('.json')).sort()) {
    const file = path.join(inbox, name); const request = JSON.parse(fs.readFileSync(file, 'utf8'));
    fs.renameSync(file, file + '.processed');
    if (request.type === 'close') {
      if (status.state !== 'idle') { announce('Close refused: session is not idle.'); continue; }
      closing = true; child.stdin.end(); setTimeout(() => child.kill('SIGTERM'), 1000).unref();
    } else if (request.type === 'abort') stop();
    else if (request.type === 'prompt') {
      if (!ready || status.state !== 'idle' || limited || status.contextPercent >= 30 || status.cost >= budget) {
        announce('Follow-up refused: session is not idle or has reached its limits.'); continue;
      }
      status.state = 'running'; save(); send('prompt', { message: request.message });
    } else if (request.type === 'steer' && ready) send('steer', { message: request.message });
  }
}, 2000);
const heartbeat = setInterval(() => announce(`Pi ${status.state}: ${status.toolCalls} tools, $${status.cost ?? '?'}, context ${status.contextPercent?.toFixed(1) ?? '?'}%.`), 60000);
child.on('error', error => { status.error = error.message; status.state = 'failed'; save(); clearInterval(timer); clearInterval(heartbeat); });
child.on('exit', (code, signal) => { status.state = 'closed'; status.exitCode = code; status.signal = signal; save(); clearInterval(timer); clearInterval(heartbeat); announce('Pi process closed.'); });
send('get_state');
