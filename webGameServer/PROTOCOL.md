# ESP32 tic-tac-toe protocol v1

Connect to `ws://<COMPUTER_LAN_IP>:8080/ws` (or the configured port).
Messages are JSON text objects with `v:1`. Send exactly the fields shown.
The server owns the board, legal moves, turns, results, and difficulty-based
computer decisions. Human is X; computer is O. X starts every new game.
Indices are 0–8, row-major from the top left; empty cells are `""`.

## Client messages

```json
{"v":1,"type":"hello","role":"esp"}
{"v":1,"type":"hello","role":"monitor"}
{"v":1,"type":"move","cell":4,"revision":0}
{"v":1,"type":"reset","revision":2}
{"v":1,"type":"difficulty","difficulty":"easy","revision":2}
{"v":1,"type":"sync"}
```

Send one hello before any other command. Only one active ESP is allowed;
a second receives `CONTROLLER_BUSY` and closes with WebSocket code 1008.
Monitors may send sync and difficulty. Only the ESP may move or reset.
Both roles may send difficulty with easy, medium, or hard and the latest revision.
An accepted difficulty command starts a fresh board, even for the same level,
and cancels pending computer work. Easy is the startup default; reset retains
the current level. Easy plays randomly; Medium takes a win or blocks an
immediate threat, otherwise plays randomly; Hard uses exhaustive minimax.
Move/reset must carry the latest state revision, a nonnegative safe integer.
Cell must be an integer from 0 to 8. Reset is allowed even during the computer
turn, provided its revision is current; it invalidates pending computer work.

## Server state

```json
{"v":1,"type":"state","revision":0,"difficulty":"easy","board":["","","","","","","","",""],"turn":"X","result":"playing","winningCells":[],"statusText":"Your turn","canMove":true}
```

- `difficulty`: `"easy"`, `"medium"`, or `"hard"`.
- `turn`: `"X"`, `"O"`, or `null` when finished.
- `result`: `"playing"`, `"X_won"`, `"O_won"`, or `"draw"`.
- `winningCells`: three winning indices, or `[]`.
- `statusText`: `"Your turn"`, `"Computer thinking"`, `"You win"`,
  `"Computer wins"`, or `"Draw"`.
- `canMove`: true only while playing on X's turn.
- `revision`: starts at 0, increases once per human move, computer move,
  accepted difficulty command, or accepted reset (even an empty-board reset). Never resets within a process.

Complete states are broadcast to all registered clients after hello, sync,
reset, difficulty, each accepted human move, and each computer move. After a nonterminal
human move the first state has `turn:"O"`, `canMove:false`, and
`statusText:"Computer thinking"`; then the selected strategy runs and the computer state
follows. A terminal human move skips the computer turn. These states can
arrive close together; no artificial thinking delay is imposed.

The server serializes mutations on its event loop. Commands are checked
against the state at processing time. Do not queue speculative moves or resets.
Game state survives ESP disconnection; computer work may finish while it is
disconnected. On reconnect send hello, accept the current state, and do not
replay old commands. Restarting the server creates a fresh game at revision 0.

## Errors

```json
{"v":1,"type":"error","code":"INVALID_MOVE","message":"Cell already occupied"}
```

Errors never change the board. Recoverable errors are followed by the current
state sent to the requesting socket. That error-state response does not register
the socket: hello is still required. `CONTROLLER_BUSY` instead closes the socket.

| Code | Meaning |
| --- | --- |
| INVALID_JSON | Text cannot be parsed as JSON |
| INVALID_MESSAGE | Wrong shape/version, unknown type, invalid revision, or binary message |
| HELLO_REQUIRED | Command received before hello |
| ALREADY_REGISTERED | Repeated hello |
| CONTROLLER_BUSY | Another active ESP exists |
| FORBIDDEN | Monitor attempted move/reset |
| STALE_REVISION | Command revision differs from current revision |
| INVALID_MOVE | Noninteger/out-of-range cell or occupied cell |
| OUT_OF_TURN | Not X's turn, or game has finished |

Messages are limited to 4096 bytes; oversized payloads close with code 1009.
Transport-level invalid frames may close the connection instead of producing
a JSON error. Reconnect and hello to recover.

## Monitor logs

```json
{"v":1,"type":"log","direction":"in","role":"esp","message":{"v":1,"type":"move","cell":4,"revision":0}}
```

Only monitors receive log envelopes. Direction is `"in"` or `"out"` relative
to the server; role is `"esp"` or `"monitor"`. `message` contains the protocol
message; malformed JSON from a registered client is represented as its raw
text string. Logs are never recursively logged. Broadcasts have one outgoing
entry per recipient. Unregistered traffic without an identifiable role and
transport-level failures are not included. A newly connecting monitor starts
receiving logs when registered; no historical log replay is provided.
The page keeps the latest 100 entries received, with local receipt time,
direction, and client role. Browser/server connection status is shown separately.
