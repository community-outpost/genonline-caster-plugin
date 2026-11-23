# Generals Online caster plugin

`genonline_caster`: a local named pipe that carries live match data between the game and programs such as caster overlays and stats tools.

## Requirements

- `genonline-plugin-sdk`, by default in `../genonline-plugin-sdk`.
- CMake 3.21+ and MSVC or clang-cl on Windows.

## Target

Windows x86 only (`genonline_caster.dll`), the platform the game ships on. The plugin links the C runtime statically and imports only `KERNEL32.dll` and `ADVAPI32.dll`.

## Build

```sh
cmake --preset <preset>
cmake --build --preset <preset>
ctest --preset <preset>
```

| Preset | Compiler |
|---|---|
| `win32-msvc` | MSVC |
| `win32-clang` | clang-cl |

`win32-clang` needs the Visual Studio component "MSBuild support for LLVM (clang-cl) toolset". Output goes to `build/<preset>/`.

`build/<preset>/plugins/genonline_caster/` then holds the plugin and the unsigned `plugin.manifest`. Sign it as the SDK describes.

| Cache variable | Default |
|---|---|
| `GENONLINE_PLUGIN_SDK_DIR` | `../genonline-plugin-sdk` |
| `GENONLINE_CASTER_VERSION` | `2026.11.0` |

## Loading

The plugin ships with the client in `plugins\genonline_caster\` and is off by default. The game loads it at startup when `%LOCALAPPDATA%\genonline\settings.client.json` sets `"plugins": { "caster": "genonline_caster" }`. Headless runs and replay simulation open no pipe.

The game encodes and decodes every message. The plugin only carries the frames and never reads their bodies. The messages are described in `GenOnline/docs/local-api.md` of the game repository.

## Pipe

- Name: `\\.\pipe\generalsonline-caster-<pid>`, where `<pid>` is the game process id.
- Byte mode duplex pipe. Remote clients are refused, and only the user the game runs as may connect. When that access rule cannot be set up, no pipe is created.
- The game sets the reader limit when it starts the listener (1 to 16). A reader beyond the limit gets `ERROR_PIPE_BUSY`.
- Each reader has its own id, which is never reused, its own 1 MiB outbound queue and its own sequence numbers. Writes never wait for a reader, so a slow reader only loses its own frames.

Outbound frames have a 17 byte little endian header: u32 body length, u8 frame type, u32 sequence number, u64 timestamp in microseconds.

- A reader gets nothing until the game sends it a HELLO frame. A frame for `GENONLINE_PLUGIN_LOCAL_API_ALL_READERS` goes to every reader that has had its HELLO.
- A frame that does not fit is dropped whole, and its sequence number is skipped so the reader sees the gap. PERIODIC frames leave the last 64 KiB of a queue to the other frame types.
- When a LIFECYCLE frame does not fit, that reader's stream ends. The game is asked for a new HELLO once half its queue is free again.
- Bodies are at most 256 KiB.

Inbound frames have a 5 byte header: u32 little endian body length and u8 frame type.

- A body length of 0 or above 4096 drops the reader. Frames before the reader's HELLO are dropped.
- Requests (`0x11`): at most 20 per second, bursts of 40, 16 waiting. A request over the limit reaches the game flagged `GENONLINE_PLUGIN_LOCAL_API_INBOUND_RATE_LIMITED`, so it can answer with a refusal (up to 4 waiting).
- Chat sends (`0x10`): up to 8 waiting. Other frame types are skipped.
