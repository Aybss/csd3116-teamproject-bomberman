# Blast Arena

A real-time, distributed and secure **Bomberman-style** multiplayer game in C, built on **Cyclone DDS**, **OpenGL** and **GLFW**.
CSD3116 Low-Level Programming team project, extended from the course baseline (`server.c` / `client.c`).

## Features

- 2–5 players on a 19×13 tile map, one authoritative server
- Randomly generated maps: indestructible pillars, destructible crates, hidden power-ups (extra bomb, bigger blast, speed)
- Chain reactions, round timer, scores, lobby -> countdown -> playing -> winner
- Smooth movement: 60 Hz server simulation, 30 Hz snapshots, jitter-buffered interpolation on the server's timeline (no rubberbanding)
- Players can **join and leave at any time** (clean exit, crash or network loss)
- **DDS Security**: certificate authentication, encrypted traffic, per-topic access control
- Server rejects inputs that try to control someone else's player (anti-spoofing)
- Headless **AI bot** (pathfinding + danger avoidance) for testing, load testing and the security demo
- Unit tests for the game rules and the interpolation (116 checks)

## Requirements

WSL with Ubuntu 22.04 (Cyclone DDS 0.8.2) or 24.04 (Cyclone DDS 0.10.4), or macOS. 

## Game rules

1. A round starts when at least 2 players are connected: a new random map is generated and everyone is placed in a corner (3 s countdown).
2. Each player starts with **1 bomb** and a blast range of **2 tiles**.
3. A bomb explodes after **2.5 s** in a cross of flames. Flames stop at walls, destroy the first crate they hit, and set off any bomb they reach (chain reaction).
4. A player standing in a flame is out.
5. About a third of crates hide a power-up, revealed when the crate is destroyed:
   - **B** – one more bomb at a time (max 5)
   - **F** – one more tile of blast range (max 8)
   - **S** – walk faster (max 4)
   Flames destroy uncollected power-ups.
6. The last player standing wins the round and scores a point. If nobody survives, or the 2:30 timer runs out, it is a draw.
7. Players who join mid-round watch as ghosts until the next round.
