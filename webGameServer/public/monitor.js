'use strict';
const connection = document.getElementById('connection');
const status = document.getElementById('status');
const details = document.getElementById('details');
const messages = document.getElementById('messages');
const difficultySelect = document.getElementById('difficulty');
const newGame = document.getElementById('new-game');
let activeSocket;
let currentRevision;
document.getElementById('difficulty-form').addEventListener('submit', event => {
  event.preventDefault();
  if (activeSocket?.readyState !== WebSocket.OPEN || currentRevision === undefined) return;
  activeSocket.send(JSON.stringify({ v: 1, type: 'difficulty', difficulty: difficultySelect.value, revision: currentRevision }));
  newGame.disabled = true;
});
const cells = Array.from({ length: 9 }, (_, index) => {
  const cell = document.createElement('div');
  cell.className = 'cell';
  cell.setAttribute('aria-label', `Cell ${index}: empty`);
  document.getElementById('board').append(cell);
  return cell;
});
function connect() {
  const socket = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws`);
  activeSocket = socket;
  socket.addEventListener('open', () => {
    connection.textContent = 'Connected to server';
    socket.send(JSON.stringify({ v: 1, type: 'hello', role: 'monitor' }));
  });
  socket.addEventListener('message', event => {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    if (message.type === 'state') {
      currentRevision = message.revision;
      difficultySelect.disabled = false;
      newGame.disabled = false;
      if (difficultySelect.dataset.current !== message.difficulty) {
        difficultySelect.value = message.difficulty;
        difficultySelect.dataset.current = message.difficulty;
      }
      status.textContent = message.statusText;
      details.textContent = `Difficulty: ${message.difficulty} � Revision ${message.revision} · Turn: ${message.turn || 'finished'} · ${message.result}`;
      cells.forEach((cell, index) => {
        cell.textContent = message.board[index];
        cell.setAttribute('aria-label', `Cell ${index}: ${message.board[index] || 'empty'}`);
        cell.classList.toggle('winning', message.winningCells.includes(index));
      });
    } else if (message.type === 'log') {
      const row = document.createElement('tr');
      for (const value of [new Date().toLocaleTimeString(), message.direction, message.role, JSON.stringify(message.message)]) {
        const td = document.createElement('td');
        td.textContent = value;
        row.append(td);
      }
      messages.prepend(row);
      while (messages.children.length > 100) messages.lastElementChild.remove();
    } else if (message.type === 'error') {
      connection.textContent = `${message.code}: ${message.message}`;
    }
  });
  socket.addEventListener('error', () => { connection.textContent = 'Connection error'; });
  socket.addEventListener('close', () => {
    currentRevision = undefined;
    difficultySelect.disabled = true;
    newGame.disabled = true;
    connection.textContent = 'Disconnected · board may be stale · reconnecting…';
    setTimeout(connect, 2000);
  });
}
connect();
