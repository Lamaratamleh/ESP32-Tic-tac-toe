'use strict';

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { WebSocketServer, WebSocket } = require('ws');

const port = Number(process.env.PORT || 8080);
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  throw new Error('PORT must be an integer from 1 to 65535');
}
const files = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/monitor.js': ['monitor.js', 'text/javascript; charset=utf-8'],
  '/style.css': ['style.css', 'text/css; charset=utf-8'],
};
const server = http.createServer((req, res) => {
  const route = (req.url || '').split('?')[0];
  const file = Object.hasOwn(files, route) ? files[route] : null;
  if (!file || !['GET', 'HEAD'].includes(req.method)) {
    res.writeHead(404).end('Not found');
    return;
  }
  fs.readFile(path.join(__dirname, 'public', file[0]), (error, data) => {
    if (error) return void res.writeHead(500).end('Unable to read page');
    res.writeHead(200, { 'Content-Type': file[1], 'Cache-Control': 'no-store' });
    res.end(req.method === 'HEAD' ? undefined : data);
  });
});
const wss = new WebSocketServer({ noServer: true, maxPayload: 4096 });
server.on('upgrade', (req, socket, head) => {
  if (req.url !== '/ws') {
    socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
    return;
  }
  wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws));
});

const lines = [
  [0, 1, 2], [3, 4, 5], [6, 7, 8],
  [0, 3, 6], [1, 4, 7], [2, 5, 8], [0, 4, 8], [2, 4, 6],
];
let board = Array(9).fill('');
let revision = 0;
let difficulty = 'easy';
let turn = 'X';
let controller = null;
let generation = 0;

function outcome(cells) {
  for (const line of lines) {
    const mark = cells[line[0]];
    if (mark && line.every(i => cells[i] === mark)) {
      return { result: `${mark}_won`, winningCells: line };
    }
  }
  return { result: cells.includes('') ? 'playing' : 'draw', winningCells: [] };
}
function state() {
  const end = outcome(board);
  const statusText = {
    X_won: 'You win', O_won: 'Computer wins', draw: 'Draw',
    playing: turn === 'X' ? 'Your turn' : 'Computer thinking',
  }[end.result];
  return {
    v: 1, type: 'state', revision, difficulty, board: [...board],
    turn: end.result === 'playing' ? turn : null,
    ...end, statusText, canMove: end.result === 'playing' && turn === 'X',
  };
}
function rawSend(ws, message) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
}
function log(direction, role, message) {
  if (!role) return;
  const envelope = { v: 1, type: 'log', direction, role, message };
  for (const client of wss.clients) {
    if (client.role === 'monitor') rawSend(client, envelope);
  }
}
function send(ws, message) {
  if (ws.readyState !== WebSocket.OPEN) return;
  rawSend(ws, message);
  log('out', ws.role, message);
}
function broadcastState() {
  const message = state();
  for (const client of wss.clients) {
    if (client.role) send(client, message);
  }
}
function fail(ws, code, message) {
  send(ws, { v: 1, type: 'error', code, message });
  send(ws, state());
}

// Ordinary exhaustive minimax; O maximizes and X minimizes.
function minimax(cells, player) {
  const result = outcome(cells).result;
  if (result !== 'playing') return { score: { O_won: 1, X_won: -1, draw: 0 }[result] };
  let best = { score: player === 'O' ? -Infinity : Infinity, cell: -1 };
  for (let cell = 0; cell < 9; cell++) {
    if (cells[cell]) continue;
    cells[cell] = player;
    const { score } = minimax(cells, player === 'O' ? 'X' : 'O');
    cells[cell] = '';
    if ((player === 'O' && score > best.score) || (player === 'X' && score < best.score)) {
      best = { score, cell };
    }
  }
  return best;
}
function computerMove(cells, level, random = Math.random) {
  const available = cells.flatMap((mark, cell) => mark ? [] : [cell]);
  if (level === 'hard') return minimax([...cells], 'O').cell;
  if (level === 'medium') {
    // Take a win first, then block an immediate human win.
    for (const mark of ['O', 'X']) {
      for (const cell of available) {
        const candidate = [...cells];
        candidate[cell] = mark;
        if (outcome(candidate).result === `${mark}_won`) return cell;
      }
    }
  }
  return available[Math.floor(random() * available.length)];
}
function scheduleComputer() {
  const token = generation;
  const expectedRevision = revision;
  setImmediate(() => {
    if (token !== generation || revision !== expectedRevision || turn !== 'O') return;
    const cell = computerMove(board, difficulty);
    board[cell] = 'O';
    revision++;
    turn = outcome(board).result === 'playing' ? 'X' : null;
    broadcastState();
  });
}
function hasKeys(message, keys) {
  return Object.keys(message).length === keys.length && keys.every(key => Object.hasOwn(message, key));
}

wss.on('connection', ws => {
  ws.role = null;
  ws.on('error', error => console.warn('WebSocket error:', error.message));
  ws.on('close', () => { if (controller === ws) controller = null; });
  ws.on('message', (data, isBinary) => {
    if (isBinary) return fail(ws, 'INVALID_MESSAGE', 'Use JSON text messages');
    let message;
    try { message = JSON.parse(data.toString('utf8')); }
    catch {
      log('in', ws.role, data.toString('utf8'));
      return fail(ws, 'INVALID_JSON', 'Malformed JSON');
    }
    const object = message && typeof message === 'object' && !Array.isArray(message);
    const claimedRole = object && message.type === 'hello' && ['esp', 'monitor'].includes(message.role)
      ? message.role : null;
    log('in', ws.role || claimedRole, message);
    if (!object || message.v !== 1 || typeof message.type !== 'string') {
      return fail(ws, 'INVALID_MESSAGE', 'Expected an object with v:1 and a type');
    }
    if (message.type === 'hello') {
      if (!hasKeys(message, ['v', 'type', 'role']) || !claimedRole) {
        return fail(ws, 'INVALID_MESSAGE', 'Hello requires role esp or monitor');
      }
      if (ws.role) return fail(ws, 'ALREADY_REGISTERED', 'Hello already received');
      if (claimedRole === 'esp' && controller && controller.readyState === WebSocket.OPEN) {
        rawSend(ws, { v: 1, type: 'error', code: 'CONTROLLER_BUSY', message: 'An ESP controller is already connected' });
        log('out', 'esp', { v: 1, type: 'error', code: 'CONTROLLER_BUSY', message: 'An ESP controller is already connected' });
        ws.close(1008, 'Controller already connected');
        return;
      }
      ws.role = claimedRole;
      if (ws.role === 'esp') controller = ws;
      broadcastState();
      return;
    }
    if (!ws.role) return fail(ws, 'HELLO_REQUIRED', 'Send hello first');
    if (message.type === 'sync') {
      if (!hasKeys(message, ['v', 'type'])) return fail(ws, 'INVALID_MESSAGE', 'Invalid sync shape');
      broadcastState();
      return;
    }
    if (!['move', 'reset', 'difficulty'].includes(message.type)) return fail(ws, 'INVALID_MESSAGE', 'Unknown message type');
    const keys = message.type === 'move' ? ['v', 'type', 'cell', 'revision']
      : message.type === 'difficulty' ? ['v', 'type', 'difficulty', 'revision'] : ['v', 'type', 'revision'];
    if (!hasKeys(message, keys) || !Number.isSafeInteger(message.revision) || message.revision < 0) {
      return fail(ws, 'INVALID_MESSAGE', 'Invalid command shape or revision');
    }
    if (ws.role !== 'esp' && message.type !== 'difficulty') return fail(ws, 'FORBIDDEN', 'Only ESP may move or reset');
    if (message.revision !== revision) return fail(ws, 'STALE_REVISION', 'Sync to the current revision');
    if (message.type === 'difficulty' && !['easy', 'medium', 'hard'].includes(message.difficulty)) {
      return fail(ws, 'INVALID_MESSAGE', 'Difficulty must be easy, medium, or hard');
    }
    if (message.type === 'reset' || message.type === 'difficulty') {
      if (message.type === 'difficulty') difficulty = message.difficulty;
      generation++;
      board = Array(9).fill('');
      revision++;
      turn = 'X';
      broadcastState();
      return;
    }
    if (!Number.isInteger(message.cell) || message.cell < 0 || message.cell > 8) {
      return fail(ws, 'INVALID_MOVE', 'Cell must be an integer from 0 to 8');
    }
    if (turn !== 'X' || outcome(board).result !== 'playing') return fail(ws, 'OUT_OF_TURN', 'Human cannot move now');
    if (board[message.cell]) return fail(ws, 'INVALID_MOVE', 'Cell already occupied');
    // Each handler and computer callback runs synchronously on Node's event loop.
    board[message.cell] = 'X';
    revision++;
    turn = outcome(board).result === 'playing' ? 'O' : null;
    broadcastState();
    if (turn === 'O') scheduleComputer();
  });
});
server.on('error', error => { console.error(error.message); process.exitCode = 1; });
if (require.main === module) server.listen(port, '0.0.0.0', () => {
  console.log(`Monitor: http://localhost:${port}\nESP: ws://<COMPUTER_LAN_IP>:${port}/ws`);
});

module.exports = { computerMove, outcome, server, wss };
