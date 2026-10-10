const { test } = require('node:test');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { WebSocket } = require('ws');
const { computerMove, outcome, server, wss } = require('../server');

test('Easy can choose every available cell without modifying the board', () => {
  const board = ['X', '', 'O', '', '', '', '', '', ''];
  const original = [...board];
  const legal = [1, 3, 4, 5, 6, 7, 8];
  legal.forEach((cell, i) => assert.equal(computerMove(board, 'easy', () => (i + .5) / legal.length), cell));
  assert.deepEqual(board, original);
});

test('Medium wins before blocking and blocks immediate threats', () => {
  assert.equal(computerMove(['X','X','','O','O','','','',''], 'medium'), 5);
  assert.equal(computerMove(['X','X','','','O','','','',''], 'medium'), 2);
});

test('Hard never loses against any human move sequence', () => {
  function visit(board, player) {
    const result = outcome(board).result;
    assert.notEqual(result, 'X_won');
    if (result !== 'playing') return;
    const moves = player === 'O' ? [computerMove(board, 'hard')]
      : board.flatMap((mark, cell) => mark ? [] : [cell]);
    for (const cell of moves) {
      assert.equal(board[cell], '');
      const next = [...board]; next[cell] = player;
      visit(next, player === 'X' ? 'O' : 'X');
    }
  }
  visit(Array(9).fill(''), 'X');
});

test('Difficulty commands reset, validate revisions, broadcast and survive ESP reset', async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const clients = [];
  async function connect(role) {
    const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}/ws`);
    clients.push(ws);
    await once(ws, 'open');
    return command(ws, {type:'hello', role});
  }
  function command(ws, body) {
    return new Promise(resolve => {
      function receive(data) {
        const message = JSON.parse(data);
        if (message.type === 'log') return;
        ws.off('message', receive);
        resolve({ ws, message });
      }
      ws.on('message', receive);
      ws.send(JSON.stringify({v:1, ...body}));
    });
  }
  try {
    const {ws: monitor, message: initial} = await connect('monitor');
    assert.equal(initial.difficulty, 'easy');
    const {ws: esp} = await connect('esp');
    for (const difficulty of ['medium', 'hard', 'easy']) {
      const {message} = await command(monitor, {type:'sync'});
      const changed = (await command(monitor, {type:'difficulty', difficulty, revision:message.revision})).message;
      assert.equal(changed.difficulty, difficulty);
      assert.equal(changed.revision, message.revision + 1);
      assert.deepEqual(changed.board, Array(9).fill(''));
      const reset = (await command(esp, {type:'reset', revision:changed.revision})).message;
      assert.equal(reset.difficulty, difficulty);
    }
    const current = (await command(monitor, {type:'sync'})).message;
    assert.equal((await command(monitor, {type:'difficulty', difficulty:'unknown', revision:current.revision})).message.code, 'INVALID_MESSAGE');
    assert.equal((await command(monitor, {type:'difficulty', difficulty:'hard', revision:0})).message.code, 'STALE_REVISION');
    const after = (await command(monitor, {type:'sync'})).message;
    assert.equal(after.revision, current.revision);
    assert.equal(after.difficulty, current.difficulty);
  } finally {
    clients.forEach(ws => ws.terminate());
    await new Promise(resolve => wss.close(resolve));
    await new Promise(resolve => server.close(resolve));
  }
});
