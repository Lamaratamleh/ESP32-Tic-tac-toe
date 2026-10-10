# ESP32 Tic-Tac-Toe

A networked Tic-Tac-Toe demo featuring an **ESP32-S3 touchscreen client**, a **Node.js game server**, and a **browser-based live monitor**. The human player uses **X** on the touchscreen; the server controls the authoritative board state and plays **O** at one of three difficulty levels.

> **Network/security scope:** This is an unauthenticated classroom/demo application intended for a **trusted local network only**. There is no TLS, user-account system, game persistence, or setup for deployment on the public internet. Anyone on the local network who takes the available ESP controller connection may play or reset the game. Do not expose port 8080 to the internet.

## Contents

- [Architecture and features](#architecture-and-features)
- [Repository structure](#repository-structure)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Game difficulty](#game-difficulty)
- [WebSocket protocol and connection behavior](#websocket-protocol-and-connection-behavior)
- [Testing and verification](#testing-and-verification)
- [Troubleshooting](#troubleshooting)
- [Security and configuration notes](#security-and-configuration-notes)

## Architecture and features

```text
  ESP32-S3 touchscreen (X)                  Browser monitor(s)
  ESP-IDF + LVGL 8.4.0                       Board, difficulty, logs
           |                                         |
           | WebSocket /ws                           | HTTP + WebSocket
           +------------------+----------------------+
                              |
                     Node.js game server
                     (authoritative state,
                      computer O, rules)
```

- **Touchscreen client:** Landscape board, game status, connection status, winning-cell highlights, and **New Game** control.
- **Server:** One shared in-memory game, legal-move validation, turns, computer decisions, results, state revisions, and controller ownership.
- **Browser monitor:** Live board/protocol messages, difficulty selection, and restart controls. Multiple browsers can observe simultaneously.
- **One ESP controller at a time:** A second ESP connection receives `CONTROLLER_BUSY`.
- **Resilience:** The ESP reconnects and synchronizes state instead of replaying previous move/reset commands.

## Repository structure

```text
ESP32-Tic-tac-toe/
├── EspGameServer/              # ESP32-S3 firmware / PlatformIO project
│   ├── platformio.ini
│   ├── partitions.csv
│   ├── src/
│   │   ├── main.c              # Display startup and app initialization
│   │   ├── game_client.c       # UI, protocol validation, networking
│   │   ├── client_config.example.h
│   │   ├── client_config.h     # Create locally; NEVER commit credentials
│   │   ├── esp_bsp.*           # Board support
│   │   ├── esp_lcd_*           # Display/touch integration
│   │   └── lv_port.*           # LVGL integration
│   └── libraries/lvgl/         # Bundled LVGL graphics library
├── webGameServer/              # Node.js server and browser monitor
│   ├── PROTOCOL.md             # Full WebSocket message contract
│   └── ...
└── README.md                   # This project-wide guide
```

`client_config.h` is **generated locally**, so it is not expected to appear in a fresh Git clone. The tree above highlights the important paths, not every file.

## Requirements

- **Hardware:** JC3248W535EN ESP32-S3 3.5-inch 320×480 capacitive-touch display module (8 MB PSRAM / 16 MB flash), USB data cable.
- **Firmware:** VS Code with PlatformIO (or PlatformIO CLI). Firmware uses `espressif32@6.6.0`, ESP-IDF 5.2.1, and LVGL 8.4.0. PlatformIO resolves the declared ESP-IDF WebSocket component (1.4.0); Wi-Fi and cJSON come from ESP-IDF. LVGL and the display/touch drivers are included in the firmware project.
- **Server:** Node.js **20 or newer** and npm.
- **Networking:** The computer and ESP on the **same reachable LAN**; ESP Wi-Fi compatible with the available network. Be mindful of guest/client isolation and Windows Firewall.

## Quick start

Commands below assume **PowerShell on Windows** and start from the repository root, `ESP32-Tic-tac-toe`. If you have not downloaded it yet:

```powershell
git clone https://github.com/Lamaratamleh/ESP32-Tic-tac-toe.git
cd ESP32-Tic-tac-toe
```

### 1. Install and run the game server

```powershell
cd webGameServer
npm install
npm start
```

Open the browser monitor on **http://localhost:8080** on the computer hosting the server. From another device on the same LAN, use `http://<COMPUTER_LAN_IP>:8080`.

The server listens on `0.0.0.0`. Leave its terminal running while using the touchscreen. Press **Ctrl+C** to stop it. **Restarting the server clears the in-memory game** and resets the revision to 0 (and difficulty to Easy).

To choose another port **before starting** the server:

```powershell
$env:PORT = '8081'
npm start
```

If you change the port, update both the ESP's WebSocket URL and the browser URL.

### 2. Find the server computer's LAN IP address

In another PowerShell terminal on the **server computer**:

```powershell
ipconfig
```

Find the `IPv4 Address` of the active Wi-Fi or Ethernet adapter (not a loopback, disconnected, or virtual adapter). Example: `192.168.1.42`.

**Do not put `localhost` or `0.0.0.0` in the ESP configuration.** The ESP needs the computer's LAN IPv4 address.

### 3. Configure ESP Wi-Fi and the server URL

From the repository root:

```powershell
cd EspGameServer
Copy-Item src\client_config.example.h src\client_config.h
notepad src\client_config.h
```

Copy the example **only once**; if `client_config.h` already exists, edit it rather than overwriting your configuration. Set your own values in the new file:

```c
#pragma once

#define CLIENT_WIFI_SSID "YOUR_WIFI_SSID"
#define CLIENT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define CLIENT_SERVER_URL "ws://192.168.1.42:8080/ws"
```

Replace the sample LAN IP with the one found in step 2. The computer and touchscreen must be able to reach one another on the network. **Never commit or share `client_config.h`**, since it contains the Wi-Fi password.

### 4. Build the ESP firmware

Run this inside `EspGameServer` (the directory containing `platformio.ini`):

```powershell
pio run -e LVGL-320-480
```

Or open `EspGameServer` as the project folder in VS Code and select **PlatformIO → Project Tasks → LVGL-320-480 → Build**.

### 5. Upload to the ESP32-S3

Connect the board with a USB **data** cable. Find its serial port and upload:

```powershell
pio device list
pio run -e LVGL-320-480 -t upload --upload-port COM5
```

**Replace `COM5` with the port actually shown on your PC** (for example, `COM3`). In VS Code you can also choose **PlatformIO → Project Tasks → LVGL-320-480 → Upload**.

No separate filesystem upload is needed. `partitions.csv` allocates **2 MB** to the firmware app within the 16 MB flash and retains the existing NVS/PHY locations. **If you edit the partition layout**, run:

```powershell
pio run -e LVGL-320-480 -t clean
pio run -e LVGL-320-480
pio run -e LVGL-320-480 -t upload --upload-port COM5
```

The regular upload flashes the firmware and partition table.

### 6. Confirm the connection and play

1. Keep `npm start` running and open the browser monitor at `http://<COMPUTER_LAN_IP>:8080`.
2. Boot the ESP32-S3 and wait until its connection status reads **Connected**.
3. Compare the board on the ESP with the board in the browser.
4. Touch an empty cell to play **X**; the server should respond with the computer's **O** move.
5. Finish a game and verify the result/winning-cell highlight; use **New Game** to restart.

## Game difficulty

The server starts at **Easy**. On the browser monitor, select a difficulty and click **Start new game** to clear the board and apply it:

| Level | Computer O behavior |
| --- | --- |
| **Easy** | Chooses randomly among legal moves. |
| **Medium** | Takes immediate wins, blocks immediate threats, otherwise chooses randomly. |
| **Hard** | Uses exhaustive minimax (perfect play); the best human result is a draw. |

An ESP-initiated reset **keeps** the current difficulty. Restarting the server resets the level to **Easy**. The ESP may also send a `difficulty` command as documented in [`webGameServer/PROTOCOL.md`](webGameServer/PROTOCOL.md).

## WebSocket protocol and connection behavior

- **WebSocket endpoint:** `ws://<COMPUTER_LAN_IP>:8080/ws` (adjust port if configured).
- **Authority:** Server owns board state, computer moves, result, and revision counter.
- **Controller handshake:** ESP sends `hello` for each connection; incoming state updates are rendered on the touchscreen.
- **Commands:** ESP sends `move`/`reset` with the **latest received revision**. Do not replay commands after a reconnect.
- **Slot behavior:** Only one ESP controller is accepted; another gets `CONTROLLER_BUSY`. Multiple browser monitors are allowed.
- **Touchscreen protection:** Input is disabled while disconnected or awaiting a server command response. Holding a cell must not submit repeat moves.
- **Timeouts:** After a five-second response timeout, the client requests synchronization once; if that also times out, it reconnects. Reconnect delays increase from approximately **1 to 10 seconds**.
- **Threading:** Network callbacks do **not** update LVGL widgets directly.
- **Server restart:** In-memory state is lost and revision becomes **0**; clients must synchronize even when the new revision is zero.
- **Browser monitor:** Keeps the latest **100** protocol log entries (newest first) per page; refreshing clears them. Timestamps are local receipt times. The connection indicator reflects browser-to-server connectivity, **not** ESP presence. Browser reconnection retries every **2 seconds**.
- **Disconnect behavior:** A clean WebSocket disconnect releases the ESP controller slot immediately; a sudden network loss may hold it until TCP detects disconnection.

See the complete protocol specification in [`webGameServer/PROTOCOL.md`](webGameServer/PROTOCOL.md).

## Testing and verification

### Server automated tests

From `webGameServer`:

```powershell
npm test
```

This tests computer difficulty strategies and WebSocket difficulty commands.

### End-to-end manual checklist

1. **First connection:** Open the monitor and connect the ESP. Verify an empty board, revision **0**, **X** turn, and live messages. Connect a second ESP and expect `CONTROLLER_BUSY`; additional monitors should still work.
2. **Moves:** Tap an empty cell. Verify exactly one move submission, a temporary O/thinking state, followed by the computer state and a revision increment per state change. Holding the cell must not send a duplicate move.
3. **Game completion:** Play until X wins, O wins, or the game draws. Confirm matching result and winning-cell highlight; human moves should be disabled after completion.
4. **Reset:** Reset using the current revision. The board should return to empty with X to move and a higher revision. Even resetting an empty board should increment the revision.
5. **Invalid commands:** During X's turn, try an occupied cell with the current revision; expect `INVALID_MOVE` and unchanged board. Also test out-of-range cell **9**, stale revision, malformed JSON, and a move from a browser monitor—none should change the game.
6. **Connection loss:** Disconnect/reconnect the ESP. Verify the existing board remains visible but disabled until synchronization, then resumes without replaying commands. Restart the server and confirm synchronization with a new game at revision **0**.
7. **Difficulty:** Test Easy, Medium, and Hard through the browser monitor, using **Start new game** to apply each selection.

### Optional: Simulate an ESP from the browser

If firmware/hardware is unavailable, open the browser monitor's **Developer Tools → Console** and run the following, **with any physical ESP controller disconnected**:

```javascript
const esp = new WebSocket(`ws://${location.host}/ws`);
let current;
esp.onmessage = e => {
  const message = JSON.parse(e.data);
  console.log(message);
  if (message.type === 'state') current = message;
};
esp.onopen = () => esp.send(JSON.stringify({v: 1, type: 'hello', role: 'esp'}));

// After receiving a suitable state, run each command separately:
esp.send(JSON.stringify({v: 1, type: 'move', cell: 4, revision: current.revision}));
esp.send(JSON.stringify({v: 1, type: 'reset', revision: current.revision}));
esp.close();
```

The move/reset examples must be executed separately after receiving a compatible current state. This is only a temporary controller simulation.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| **Build fails: missing `client_config.h` or `CLIENT_WIFI_SSID`** | Copy `src/client_config.example.h` to `src/client_config.h`, then configure credentials and LAN server URL. |
| **PlatformIO does not show Build/Upload** | Open **`EspGameServer`**, which contains `platformio.ini`, not just the repository root. |
| **Upload fails / no serial port** | Verify a USB **data** cable, board power, the detected COM port (`pio device list`), and use the correct `--upload-port`. |
| **ESP cannot connect** | Confirm `npm start` is running, correct LAN IPv4 and port, computer/ESP network reachability, and no guest/client isolation. Verify trusted-network Windows Firewall rules permit Node.js/inbound TCP on the selected port. The app does **not** change firewall settings. |
| **Screen is black** | Confirm firmware **built and uploaded successfully** first; a USB connection alone does not flash the app. If still black, inspect serial output and display initialization. |
| **Moves disabled / `CONTROLLER_BUSY`** | Confirm ESP connectivity and no other ESP has the controller slot. Wait for synchronization after reconnect. |
| **Game unexpectedly resets** | A server restart intentionally clears the in-memory game and resets difficulty to Easy. |
| **Touch or protocol mismatch** | Compare the tapped cell with the selected cell and the monitor's latest messages. |

When requesting help, provide the **first compiler/runtime error plus roughly ten surrounding lines**, or the ESP connection status and recent monitor protocol messages. **Never post Wi-Fi credentials.**

## Security and configuration notes

- Keep `EspGameServer/src/client_config.h` local and excluded from Git with `.gitignore`; publish only `client_config.example.h` with placeholder values.
- Do not commit API keys, `.env` secrets, passwords, or network credentials.
- This demo has **no authentication or TLS**. Operate it only on a trusted LAN and do not expose the server to the public internet.
- Server state and monitor logs are **not persisted**. Server restart resets the game; refreshing a browser clears that page's monitor logs.

---

### Component documentation

This top-level README consolidates the installation, configuration, usage, behavior, and checks documented in the original component guides:

- [`EspGameServer/README.md`](EspGameServer/README.md) — ESP32 touchscreen firmware details.
- [`webGameServer/README.md`](webGameServer/README.md) — Node.js server/monitor details.
- [`webGameServer/PROTOCOL.md`](webGameServer/PROTOCOL.md) — detailed message contract.
