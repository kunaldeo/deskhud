# Third-party notices

DeskHUD is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)). It bundles or is
built from the following third-party works, each under its own license.

## Fonts

Converted to LVGL bitmap fonts in `firmware/main/fonts/` by `firmware/tools/gen_fonts.sh`.

| Font | Author | License |
|---|---|---|
| Inter | Rasmus Andersson | SIL Open Font License 1.1 |
| JetBrains Mono | JetBrains | SIL Open Font License 1.1 |
| IBM Plex Sans, IBM Plex Mono | IBM | SIL Open Font License 1.1 |
| Source Serif 4 | Adobe | SIL Open Font License 1.1 |
| Material Symbols Rounded (icon glyphs) | Google | Apache License 2.0 |
| Nerd Fonts icon glyphs, from JetBrainsMono Nerd Font (the icons fastfetch prints) | Ryan L McIntyre and the Nerd Fonts contributors; glyphs from Font Awesome, Material Design Icons, Devicons, Codicons, Octicons and others | MIT (Nerd Fonts patcher); the glyph sets under their own licenses (SIL OFL 1.1, MIT, Apache 2.0), all permitting redistribution |

## Images

Rasterized into `firmware/main/images/` by `firmware/tools/gen_images.py`.

| Source | License |
|---|---|
| Microsoft Fluent UI System Icons, color set (`firmware/assets/svg/fluent/`) | MIT |
| Claude and Codex marks from Lobe Icons (`firmware/assets/svg/brand/`) | MIT (icons); the marks themselves are trademarks of Anthropic and OpenAI, used only to identify those tools |
| Hardware icons and the pi, oh-my-pi and opencode marks (`firmware/assets/svg/hw/`, `brand/pi-color.svg`, `brand/omp-color.svg`, `brand/opencode-color.svg`) | original to this project, Apache License 2.0 |

## Libraries (not vendored)

The firmware pulls these through the ESP-IDF component manager (`firmware/main/idf_component.yml`,
pinned in `firmware/dependencies.lock`). The monitoring service pulls its crates through Cargo
(`monitor/Cargo.lock`).

| Library | License |
|---|---|
| ESP-IDF | Apache License 2.0 |
| LVGL | MIT |
| espressif/esp_lvgl_adapter, esp_lcd_touch_gt911, mdns | Apache License 2.0 |
| Rust crates (tungstenite, mdns-sd, serde, clap, nvml-wrapper, …) | MIT and/or Apache 2.0, see each crate |

## Hardware

Waveshare ESP32-S3-Touch-LCD-7B board details (pin mapping, panel timings, IO expander protocol)
follow Waveshare's Apache-2.0 sample code. Waveshare is not affiliated with this project.
