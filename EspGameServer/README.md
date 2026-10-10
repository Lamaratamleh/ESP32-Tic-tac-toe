# Tic-tac-toe touchscreen client

JC3248W535EN firmware using ESP-IDF, PlatformIO and LVGL 8.4.0.
The human plays X; the server controls the board, computer moves and results.
The landscape screen shows the board, game status, connection status and New Game.

## Configuration

From a PlatformIO terminal:

```powershell
cd C:\Users\feras\Drive\EspGameServer
Copy-Item src\client_config.example.h src\client_config.h
notepad src\client_config.h
```

Copy the example only once. Enter your Wi-Fi credentials and set the server URL
 to `ws://<COMPUTER_LAN_IP>:8080/ws`. Find the computer's LAN IPv4 address with
`ipconfig`; do not use localhost. The configuration file is excluded from Git.

## Build and upload

```powershell
pio run -e LVGL-320-480
pio device list
pio run -e LVGL-320-480 -t upload --upload-port COM5
```

Replace `COM5` with your board's port. PlatformIO uses `espressif32@6.6.0` and
fetches the declared ESP-IDF WebSocket component, version 1.4.0. Wi-Fi and cJSON
come from ESP-IDF; LVGL and display/touch drivers are included locally.
No filesystem upload is needed.

`partitions.csv` allocates 2 MB for the application within the board's 16 MB
flash. NVS and PHY data retain their existing locations. After changing the
partition layout, run `pio run -e LVGL-320-480 -t clean`, rebuild, then use the
normal upload command above to flash both the firmware and partition table.

## Manual check

1. Start the server in `C:\Users\feras\Drive\webGameServer` with `npm start`
   (run `npm install` first if needed). Open `http://<COMPUTER_LAN_IP>:8080`.
2. Configure and upload the firmware. Confirm Connected and matching boards.
3. Tap an empty cell. Check that the monitor receives one move and then the
   human/computer state updates. Holding a cell must not repeat the move.
4. Finish a game, check the result and winning-cell highlight, then tap New Game.
5. Interrupt and restore connectivity. The board stays visible but disabled
   until synchronization; no move or reset is replayed. Restarting the server
   should also synchronize correctly, including revision 0.

## Code layout

- `src/main.c`: display startup and application initialization.
- `src/game_client.c`: UI, protocol validation and network task.
- `src/client_config.h`: local Wi-Fi and server settings.
- `src/esp_bsp.*`, `src/esp_lcd_*`, `src/lv_port.*`: hardware and LVGL integration.
- `libraries/lvgl/`: bundled graphics library.

Input is disabled while disconnected or waiting for a command response.
After a five-second timeout, the client requests synchronization once, then
reconnects if that also times out. Reconnect waits increase from one to ten
seconds. `CONTROLLER_BUSY` is displayed while waiting for the server's slot.
Network callbacks never update LVGL widgets directly.

If a step fails, send the first error with about ten surrounding lines, or the
ESP's connection status and the monitor's latest protocol messages. For touch
issues, describe the tapped and selected cells. Do not include Wi-Fi credentials.
