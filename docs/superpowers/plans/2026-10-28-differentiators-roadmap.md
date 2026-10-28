# Differentiators Roadmap

Five plans, one per subsystem. Each produces working, tested software on its own. Do them in this order: later plans reuse what earlier ones add.

| # | Plan | Builds on | Adds for later plans |
|---|---|---|---|
| 1 | [Station network](2026-10-28-station-network.md) | remote API (`core/include/dect2/net/remote_api.h`), automation bus, Eye, `df/position.h` | event fan-out hook, `EventRing`, `events` WebSocket subscription, per-site Eye |
| 2 | [Web client](2026-10-28-web-client.md) | plan 1's event stream, remote API | static file serving, `/api/v1/x/eye`, `/api/v1/x/msgs`, view/control tokens |
| 3 | [Site baseline and alerts](2026-10-28-site-baseline.md) | sweep scanner, automation bus, watchlists | `baseline.db`, `baseline.*` events, the `Baseline` watchlist kind |
| 4 | [Ask the Eye](2026-10-28-ask-the-eye.md) | msgdb, flights db, Eye, plan 3's baseline | briefing, tool layer, local model client |
| 5 | [Setup: station check and listening plan](2026-10-28-listening-plan.md) | existing wizard (`app/ui/wizard.cpp`), station health, propagation, pass scheduler | — |

## Decisions made here (they hold for all five plans)

- **No new third-party libraries.** HTTP, WebSocket and JSON stay in-house (`remote_api`, `ws_client.h`, `automation/json_mini.h`); storage is the bundled SQLite (`dect2_sqlite`).
- **Nothing leaves the station unless the user sets it up.** Station links, the web client on the LAN and the model endpoint are all off by default. The model endpoint must be a loopback or private address unless the user ticks "allow a remote model server".
- **The text model is not vendored.** Ask the Eye talks to a local OpenAI-compatible server the user runs (llama.cpp's `llama-server`, Ollama). The briefing works without any model.
- **TDOA / multilateration is out of scope.** No source records sample-accurate time (`IqSource` moves samples only; `adsb_feedin.h:2` says the same). It needs its own plan: read the hardware time (USRP / Soapy `time_spec` with PPS or GPSDO), stamp IQ blocks, carry the stamps in events. Cross-site **bearing** fixes (plan 1, task 5) need no shared clock and come first.
- **One user at a time per tuned channel.** Plan 2 lets many people watch at once, but only a control token can retune. Separate channels per viewer from one wide capture is a later plan.

## Out of scope for all five

TLS for station links (`ws_client.h` has none: links are for the LAN or a VPN; `wss://` is a follow-up), MQTT, cloud services, vendoring llama.cpp.
