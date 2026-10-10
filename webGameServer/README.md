# ESP32 tic-tac-toe server

One shared, in-memory game: an ESP touchscreen plays X, Node.js plays O using
three selectable difficulty levels, and browsers monitor the board and protocol messages live.
The server owns all game logic. ESP firmware is not included.

This is an unauthenticated trusted-local-network classroom demo. Anyone on
that network who connects as the available ESP controller can play/reset.
There is no persistence, TLS, account system, or internet deployment setup.

## Install and start (PowerShell)

Install Node.js 20 or newer with npm available, then run:

```powershell
cd C:\Users\feras\Drive\webGameServer
npm install
npm start
```

Open <http://localhost:8080>. To change the port before starting:

```powershell
$env:PORT = '8081'
npm start
```

The server binds to `0.0.0.0`. Use the same configured port in all URLs.
Stop with Ctrl+C. Server restart loses the game and resets revision to 0.

## Difficulty

The game starts on **Easy**. On the monitor page, choose a level and click
**Start new game** to clear the board and apply it:

- **Easy:** random legal moves, giving you opportunities to win.
- **Medium:** takes immediate wins, blocks immediate threats, otherwise plays randomly.
- **Hard:** exhaustive minimax (perfect play); your best result is a draw.

ESP resets keep the selected level. Server restarts return to Easy.
The ESP can also select a level using the `difficulty` command in PROTOCOL.md.

## Connect the ESP

Run `ipconfig` in PowerShell and find the IPv4 address of the active Wi-Fi or
Ethernet adapter (not loopback or a disconnected/virtual adapter). For example,
if it is `192.168.1.42`, configure the ESP WebSocket URL as
`ws://192.168.1.42:8080/ws`. Do not use `localhost` or `0.0.0.0` on the ESP.

Put the computer and ESP on the same network. Send ESP hello on each connection,
render received states, and send move/reset with the latest received revision.
Never replay commands after reconnect. See [PROTOCOL.md](PROTOCOL.md) for the
complete contract. Additional browsers may open `http://192.168.1.42:8080`.

If connection fails, check the address/port, server console, same-network
connectivity, guest-network/client isolation, and whether Windows Firewall
allows Node.js/inbound TCP on the selected port for the trusted network.
This project does not change firewall settings.

## Short manual checklist

1. Open the monitor and connect the ESP with hello. Confirm initial empty board,
   revision 0, X turn, and live message entries. A second ESP must receive
   `CONTROLLER_BUSY`; additional browser monitors should connect normally.
2. Play to completion. For each valid X move, inspect the log for the O/thinking
   state followed by the computer state, with a revision increment for each.
   Verify win/draw display and disabled human input after the game finishes.
3. Send reset with the current revision. Verify an empty board, X turn, and an
   increased revision. Repeat on an empty board: the revision still increases.
4. During an X turn, submit an occupied cell with the current revision. Expect
   `INVALID_MOVE` followed by unchanged state. Also try cell 9, a stale revision,
   malformed JSON, and a monitor move. None should change the board.
5. Disconnect/reconnect the ESP, sending hello without replaying commands.
   Verify the game persists. Restart the server separately to verify a fresh game.

Without firmware, use a browser developer console on the monitor page as a
temporary ESP (disconnect any actual ESP first):

```javascript
const esp = new WebSocket(`ws://${location.host}/ws`);
let current;
esp.onmessage = e => {
  const message = JSON.parse(e.data);
  console.log(message);
  if (message.type === 'state') current = message;
};
esp.onopen = () => esp.send(JSON.stringify({v:1,type:'hello',role:'esp'}));
// Run each command separately after receiving the appropriate state:
esp.send(JSON.stringify({v:1,type:'move',cell:4,revision:current.revision}));
esp.send(JSON.stringify({v:1,type:'reset',revision:current.revision}));
esp.close();
```

Monitor logs contain the latest 100 entries received by that page, newest first;
refreshing clears them. Times are local receipt times. Connection status refers
to the browser's server connection. No ESP presence field exists in the shared
protocol. The monitor automatically retries its connection every two seconds.
Abrupt network loss may leave the ESP slot occupied until TCP detects the lost
connection; clean WebSocket disconnects release it immediately.

Run `npm test` to check difficulty strategies and WebSocket difficulty commands.
