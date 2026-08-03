# VNC display for ESPHome

An ESPHome [external component](https://esphome.io/components/external_components.html) that turns
any ESPHome display surface into a VNC server. Point a VNC client at your device and you get a live
view of what the device is drawing — and clicks in the viewer arrive back as touch events.

It can act as your only display (a headless device with a screen you reach over the network), or as a
second head alongside a physical panel, so you can see and operate the real UI from your desk.

Originally written by [Clyde Stubbs](https://github.com/clydebarrow).

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Configuration: `display`](#configuration-display)
- [Configuration: `touchscreen`](#configuration-touchscreen)
- [Authentication](#authentication)
- [Using it with LVGL](#using-it-with-lvgl)
- [Memory and bandwidth](#memory-and-bandwidth)
- [What the log messages mean](#what-the-log-messages-mean)
- [Limitations](#limitations)

## Requirements

| | |
|---|---|
| **Platforms** | `esp32` and `host` only. The component runs a transmit thread, which needs FreeRTOS or pthreads. Other targets are rejected during validation. |
| **Framework** | ESP-IDF or Arduino on ESP32. |
| **Network** | Requires a configured `wifi:` or `ethernet:`. |
| **RAM** | The framebuffer is `width × height × 4` bytes. PSRAM is used when available and internal RAM otherwise, so anything above roughly 250×250 needs PSRAM. See [Memory and bandwidth](#memory-and-bandwidth). |
| **Clients** | One at a time. A second client waits in the listen backlog until the first disconnects. |

## Installation

```yaml
external_components:
  - source: github://nagyrobi/esphome-components
    components: [vnc]
```

While iterating, add `refresh: 0s` so ESPHome re-fetches the component instead of using a cached
copy. 

## Quick start

A device with no physical screen at all, drawing a clock you can view from any VNC client:

```yaml
display:
  - platform: vnc
    id: vnc_display
    dimensions: 320x240
    update_interval: 1s
    lambda: |-
      it.fill(Color(20, 20, 30));
      it.rectangle(10, 10, 300, 220, Color(255, 255, 255));
      it.filled_circle(160, 120, 40, Color(0, 160, 255));
```

That is the whole thing — with the `external_components` block above and a working `wifi:`, it
builds and runs. Add a `font:` and `it.printf(...)` once you want text.

Connect with any client to `<device-ip>:5900`, or `<device-ip>::5900` for clients that want an
explicit port. Nothing else is required — no password, no touchscreen.

## Configuration: `display`

```yaml
display:
  - platform: vnc
    id: vnc_display
    dimensions:
      width: 480
      height: 480
    port: 5900
    password: !secret vnc_password
    on_connect:
      - logger.log: "viewer attached"
    on_disconnect:
      - logger.log: "viewer gone"
```

### Options specific to this platform

- **`dimensions`** (**Required**) — the size of the virtual screen: 

  ```yaml
  dimensions:
    width: 480
    height: 480
  ```

  Each axis may be 1–32767, but the real limit is memory: see
  [Memory and bandwidth](#memory-and-bandwidth). When used as a second head alongside a physical
  panel, this must match the physical panel's size.

- **`port`** (*Optional*, integer, default `5900`) — TCP port to listen on. `5900` is the standard
  VNC port, which most clients assume when you give them a bare IP address. Use a different port if
  you run more than one VNC display on the same device.

- **`password`** (*Optional*, string) — enables VNC authentication. 1–8 ASCII characters; longer or
  non-ASCII values are rejected at compile time. Omit it entirely for no authentication, which is the
  default. See [Authentication](#authentication).

- **`on_connect`** (*Optional*, [Automation](https://esphome.io/guides/automations.html)) — runs when
  a client has finished the handshake and is about to receive its first frame. Useful for waking a
  paused UI or turning a backlight on.

- **`on_disconnect`** (*Optional*, Automation) — runs when the client goes away, for any reason
  (clean close, reset, network loss, failed password).

- **`id`** (*Optional*, ID) — an ID for this display, so a `touchscreen` or an `lvgl:` block can refer
  to it. Required in practice for anything beyond a single standalone display.

### Inherited display options

Everything from ESPHome's standard [display
component](https://esphome.io/components/display/index.html) is available:

- **`lambda`** — the drawing code, run every `update_interval`. Mutually exclusive with `pages`.
- **`pages`** / **`on_page_change`** — multiple lambdas you can switch between.
- **`update_interval`** (default `1s`) — how often `lambda` or `pages` is run, and how often any
  pending changes are flushed to the client. Leave it at the default even when something else (such
  as LVGL) is doing the drawing: the periodic flush is what recovers changes that were skipped
  because the transmit queue was momentarily full. Do not set `never` for that reason.
- **`auto_clear_enabled`** — whether the framebuffer is cleared before each redraw. Defaults to
  `true` when you set `lambda` or `pages`, and `false` otherwise. Clearing a large framebuffer every
  update is not free; if your lambda paints every pixel anyway, set `false`.
- **`show_test_card`** — draws a test pattern instead of your lambda. Handy for confirming
  dimensions, colours and channel order end-to-end without writing any drawing code.

> **`rotation` is accepted but does nothing.** It comes from the shared display schema, but this
> platform does not implement it — the framebuffer is always sent unrotated. If you need a rotated
> image, rotate at the source: in LVGL (see below), or by drawing rotated in your lambda. Setting it
> will not produce an error, it will simply have no effect, other than disabling the fast pixel
> conversion paths and making updates slower.

## Configuration: `touchscreen`

Optional. Add it to turn mouse clicks in the VNC client into ESPHome touch events, so a remote viewer
can actually operate the UI.

```yaml
touchscreen:
  - platform: vnc
    id: vnc_touch
    display: vnc_display
    on_release:
      - logger.log: "clicked"
```

- **`display`** (*Optional*, ID) — the display these touches belong to. Auto-detected if your config
  has exactly one display; **required as soon as you have more than one**, otherwise you get
  `Too many candidates found for 'display'`.
- **`id`** (*Optional*, ID) — an ID for the touchscreen, for use in `lvgl:` or automations.

All the standard [touchscreen](https://esphome.io/components/touchscreen/index.html) options work.

## Authentication

With no `password:` the server advertises RFB security type 1 ("None") and any client that can reach
the port gets in.

Setting `password:` switches to security type 2, VNC Authentication: the server sends a random
challenge, the client returns it encrypted with the password, and the server verifies it. After a
wrong password the connection is closed and new connections are refused for three seconds, to slow
down guessing.

```yaml
display:
  - platform: vnc
    dimensions: 480x480
    password: !secret vnc_password
```

**Please read this part.** VNC authentication is weak by modern standards:

- Only the **first 8 characters** are used — that is a hard limit of the scheme, not of this
  component. Longer passwords are rejected at compile time rather than silently truncated, because a
  silently shortened password is worse than an error.
- It uses single DES, which is long broken. Someone who can capture the handshake on your network can
  recover the password offline without much trouble.
- Only the handshake is protected. **Everything after it — your entire screen, and every tap — is
  sent in clear text.**

So treat it as a lock on a door, not a safe: it keeps casual visitors on your LAN out of your
thermostat UI. If you need real confidentiality, or you were thinking of forwarding port 5900 through
your router, don't — put it behind a VPN or an SSH tunnel instead.

The password is marked sensitive, so it is masked in the ESPHome dashboard and redacted from config
dumps. Use `!secret` for it anyway.

## Using it with LVGL

This is the interesting case: LVGL renders once and flushes to every display you list, so a physical
panel and a VNC display stay in sync automatically, both showing the same UI.

```yaml
display:
  - platform: st7701s
    id: my_display
    dimensions: { width: 480, height: 480 }
    # ... your panel's pins, timings and init sequence (abbreviated here);
    # an RGB panel like this one also needs its own `spi:` bus for the init sequence
    auto_clear_enabled: false
    update_interval: never

  - platform: vnc
    id: vnc_display
    dimensions: { width: 480, height: 480 }
    on_connect:
      - lvgl.resume:
      - lvgl.widget.redraw:

touchscreen:
  - platform: gt911
    id: my_touch
    display: my_display
  - platform: vnc
    id: vnc_touch
    display: vnc_display

lvgl:
  displays:
    - my_display
    - vnc_display
  touchscreens:
    - my_touch
    - vnc_touch]
  color_depth: 16
  buffer_size: 100%
  # ... your widgets
```

Points worth knowing:

- **Both displays must be the same size.** LVGL renders one buffer and sends the same rectangles to
  every display in the list.
- **Set `rotation` in the `lvgl:` block, not on either display.** ESPHome enforces this and will tell
  you so: *"use of 'rotation' in the display config is not compatible with LVGL, please set rotation
  in the LVGL config instead."* LVGL then rotates in software and both heads come out correct.
- **`color_depth: 16` is the fast path.** The component converts RGB565 straight into its
  framebuffer with a lookup table. Other depths and channel orders fall back to a per-pixel
  conversion that is several times slower.
- **Don't set `lambda` or `pages` on the VNC display** when LVGL drives it. LVGL does the drawing;
  `auto_clear_enabled` then correctly defaults to `false`.
- **Give the VNC touchscreen an explicit `display:`.** With two displays in the config, ID
  auto-detection is ambiguous and validation fails.
- Use `on_connect` to wake things up, as above, if you pause LVGL or dim a backlight when idle. Note
  it fires for *any* viewer, including one that just failed authentication and reconnected.

## Memory and bandwidth

The framebuffer is 2 bytes per pixel (RGB565), allocated once at startup:

| Dimensions | Framebuffer |
|---|---|
| 320×240 | 150 KiB |
| 480×480 | 450 KiB |
| 800×480 | 750 KiB |

PSRAM is preferred, with internal RAM as a fallback. If the allocation fails the component logs an
error naming the size it wanted and marks itself failed, so a missing `psram:` block shows up
immediately rather than as a mystery crash. On top of that it uses a fixed 4 KiB staging buffer and a
1.6 KiB rectangle queue in internal RAM.

Wire traffic is the same 2 bytes per pixel, uncompressed — only raw encoding is implemented. A full
480×480 refresh is 450 KiB. Only changed rectangles are sent, so a UI with a few updating labels
costs very little; something animating the whole screen costs a lot.

**If your device also drives an RGB parallel panel** (ST7701S and friends on an ESP32-S3), be aware
that the VNC framebuffer and the panel's scan-out share PSRAM bandwidth. A connected client streaming
frames can starve the panel's DMA and make the physical display flicker. Things that help: keep
`color_depth: 16` in LVGL, which matches the framebuffer format exactly and makes each flush a plain
`memcpy` with no per-pixel work; avoid animating large areas; and keep the panel's `pclk_frequency` no
higher than it needs to be.

## What the log messages mean

Set `logger: level: DEBUG` to see the session lifecycle.

| Message | Meaning |
|---|---|
| `Listening on port 5900` | Socket is up and waiting. Appears once the network is connected. |
| `Client connected` | TCP connection accepted; handshake starting. |
| `Client authenticated` | Correct password. Only with `password:` set. |
| `Authentication failed, dropping client` | Wrong password. New connections refused for 3 s. |
| `Client offered N encodings` | Normal. Only raw encoding is implemented, so the list is ignored. |
| `Connection closed by peer while writing (errno 104)` | Debug-level, and **normal**. A client that closes while frames are in flight still has unread data buffered, and TCP requires it to answer with a reset. Only an idle client produces a clean close. |
| `Client disconnected` | Session over, resources released. |
| `Client requested unsupported pixel format` | The client asked for a format other than 32bpp true colour. The server keeps sending its own format; colours may look wrong. Rare — most clients accept what the server advertises. |
| `Could not allocate N bytes for the display buffer - PSRAM is required at this size` | Add a `psram:` block, or reduce `dimensions`. |
| `Socket write failed: errno N` (warning) | A real socket error, as opposed to the routine disconnect above. |

## Limitations

- **One client at a time.**
- **Raw encoding only** — no compression. Simple and low-CPU, but bandwidth-hungry.
- **RFB protocol 3.3.** Fine for every common client (Remmina, TigerVNC, RealVNC, TightVNC,
  macOS Screen Sharing).
- **No keyboard input.** Key events are received and logged at verbose level, but not delivered
  anywhere. Only pointer events become ESPHome touch events.
- **`rotation:` on the display is ignored** — see the note in [Inherited display options](#inherited-display-options).
- **Clipboard is discarded** — text pasted into the viewer is read off the wire and thrown away.
- **The client's requested pixel format is not honoured** — the server always sends 16bpp
  big-endian RGB565 true colour. This matches LVGL's `color_depth: 16` exactly, so the common
  path involves no pixel conversion at all.
- **Frames are not double-buffered.** A client can occasionally see a partially redrawn region if a
  flush lands mid-draw. In practice this shows up as brief tearing, not corruption.
