# Two-console netplay

This build adds two-player rollback netplay between **two installations of FBNeo PS5**. Both consoles run the game locally and exchange controller inputs over a direct TCP connection. The host is Player 1 and the guest is Player 2. Each console uses its first controller, with that controller's existing button layout.

## Start a game

1. Install the same FBNeo PS5 netplay build on both consoles. Load the same ROM set, including matching parent/BIOS ROMs, and select the same DIP settings before connecting.
2. Open the pause menu with **L3 + R3**, then select **Netplay**.
3. On the host, select **Host a game (Player 1)**. The default TCP port is **55435**.
4. On the guest, enter the host's IPv4 address and the same port, then select **Join a game (Player 2)**.
5. Wait for the status to show Connected. Return from the Netplay screen and resume on **both** consoles. Insert coins and press Start using your normal button layout.
6. To leave, open Netplay and choose **Disconnect**, or close the game. The other machine detects the disconnection.

On a LAN, use the host PS5's local IPv4 address. Across the Internet, the host needs incoming TCP access to the selected port, usually a router forwarding rule to its PS5. A shared routed VPN is another option. There is no automatic UPnP setup, relay service, public room directory or RetroArch lobby integration in this implementation. Use trusted peers; the direct connection is not encrypted.

## Synchronization and recovery

- Peers compare a fingerprint of the emulation/frontend sources, app/compiler version, the actually loaded ROM CRCs and DIP settings before exchanging game state. A matching filename alone is insufficient.
- The host sends the initial game state. Each machine retains bounded rollback snapshots, predicts missing remote input and replays affected frames when the actual input differs. Only the final replay output reaches the frontend's presentation/audio queues.
- The prediction window is at most eight frames and the snapshot ring is bounded to 64 MiB. A game whose full state exceeds 16 MiB is refused. Excessive delay stalls emulation until input arrives instead of growing memory without limit.
- Confirmed-state checksums are exchanged every 60 frames. A mismatch stops the session with a desynchronization message.
- Opening the pause menu pauses the session for both players. Both must resume to continue. State loads/saves, reset, power cycle, service/test switches, DIP edits and settings changes are blocked while a session is active.
- Periodic NVRAM and high-score writes are suspended during netplay. Disconnecting restores each console's pre-session offline game state, so the guest does not overwrite its saves with the host's NVRAM. **Online progress is not saved by this version.**
- Connection, synchronization and peer-loss timeouts keep the frontend responsive. Closing the game closes the listener/connection and releases the rollback snapshots.

## Supported games and limits

The game must expose at least two players and reversible, deterministic states. A short state/replay probe runs before a session starts. Games with writable hard disks are rejected because their disk writes are not part of the rollback state. Passing the initial probe does not prove every driver is deterministic throughout an entire game; ongoing checksums detect divergence.

The implementation uses its own versioned protocol. It does **not** connect to RetroArch clients, Kaillera, Fightcade or the original arcade machine's network/link mode. No game ROMs are transferred between players.

## Build and test

`make test-netplay` runs real TCP loopback tests with ASan/UBSan on a deterministic test machine: initial state transfer, delayed input/rollback, offline-reference comparison, pause, disconnect, incompatible peers, large/partial transfers, desynchronization and malformed-packet limits.

`python3 tools/netplay_build_id.py` produces the source fingerprint used by the build. The normal host/PS5 targets include the new menu and network module. Use the same source/configuration and distribute the same compiled ELF to both players.

Host loopback and scripted frontend tests do not replace testing two actual PS5 consoles. Native socket permissions, real network latency, game-specific determinism, audio behavior and long sessions must be verified on hardware before describing netplay as console-tested.

The rollback approach follows the general model documented by [Libretro](https://docs.libretro.com/development/retroarch/netplay/). The new implementation is independent and does not copy RetroArch's protocol implementation.

See [validation results](NETPLAY-VALIDATION.md) for the checks performed and the remaining hardware test.
