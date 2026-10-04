# Changelog

All notable changes to ShackBook are recorded here. Entries before the
rename below refer to the project under its former name, ShackLog, and
are left unedited as the historical record. The format follows
[Keep a Changelog](https://keepachangelog.com/), and the project uses
[semantic versioning](https://semver.org/). Releases are cut by pushing a
`vX.Y.Z` tag; CI then builds the Windows installer + zip, Linux AppImage, and
macOS DMG and attaches them to the GitHub release.

## [Unreleased]

### Added
- **Intel Mac build.** Releases now include `ShackBook-<version>-macos-x86_64.dmg`
  alongside the Apple Silicon DMG. It runs on macOS 12 or later. It is built on the
  same runner as the Apple Silicon DMG, and CI checks the binary really is Intel code
  before packaging it.

## [0.7.0] - 2026-08-27

### Changed
- **Renamed from ShackLog to ShackBook.** A UK amateur radio logging program
  has used the name ShackLog since at least 2005, holds `shacklog.co.uk` and
  `shacklog.com`, and is listed on DXZone. Continuing under that name would
  have been confusing for operators and unfair to the people who had it
  first, so this project moved rather than compete for the name.
- **Your logbooks move with you.** On first run, an existing
  `…/G0JKN/ShackLog/` data directory is **copied** — not moved — to
  `…/G0JKN/ShackBook/`, and the app tells you it has done so. The originals
  are left untouched; delete them by hand once you are satisfied. If you have
  already started logging under the new name, nothing is overwritten.
- ADIF exports now carry `PROGRAMID: ShackBook` and Cabrillo exports
  `CREATED-BY: ShackBook`. Logs exported before the rename keep the old name,
  which is correct — that is what produced them.

### Added
- **Session Map** (`Maps → Session Map…`) — plots one operating session's QSOs
  with great-circle paths from your own grid, coloured by band. A session is
  derived: a gap of more than two hours starts a new one, which keeps an
  evening crossing midnight UTC in one piece. Says how many QSOs it could not
  place rather than dropping them silently.
- **QSO Party mode** (`Tools → QSO Party…`) — a worked/needed county table for
  state QSO parties, with the needed counties listed rather than merely
  counted, and a calendar of what is running now or coming up.
- **US county reference list** — 3235 counties from US Census 2020 FIPS data,
  bundled rather than fetched so it works at a field site with no internet.
  Includes Virginia's independent cities, Louisiana parishes, Alaska boroughs
  and census areas, and Puerto Rico municipios.
- **Contest definitions and calendar** — what a contest's exchange is, and
  when it runs. The calendar stores recurrence rules ("the second full weekend
  of August") and computes dates, so it stays correct in years nobody updates
  the file. Every bundled definition states whether its exchange has been
  confirmed against the sponsor's published rules; all currently read
  UNCONFIRMED.

## [0.6.0] - 2026-08-05

### Added
- **Find radios** (Tools → Find radios…, and a button in Settings → TCI) — ShackLog
  now looks for TCI servers instead of asking you to type a host and port. Pick one
  from the list and it connects.
  - Only servers that answer a real TCI handshake are offered. A port that merely
    accepts a connection is **not** a radio, and listing one would be worse than
    listing nothing — other software in a busy shack sits on the same ports.

- **Radios without TCI.** Icom, Yaesu, Kenwood and anything else
  [Hamlib](https://hamlib.github.io/) drives can now be followed, through its
  `rigctld` program. Choose it under **Settings → TCI → Follow radio via**; host,
  port and nickname all follow the choice, so switching back to TCI finds its own
  settings again.
  - Hamlib reports the **radio's own model** ("IC-9700"), where TCI reports the
    application name. So contacts made through a CAT radio are attributed correctly
    with no nickname needed — the ambiguity that made nicknames necessary for TCI
    simply does not arise.
  - Frequency and mode arrive on the same path as TCI, so everything downstream —
    band, dupe checking, logging — behaves identically whichever kind of radio is
    attached.
  - ⚠ **Hamlib is not included.** It is a separate project under a different licence,
    and if you run WSJT-X or fldigi you very likely have it already. ShackLog finds
    it automatically, says plainly when it cannot, and lets you point at it — see
    *Following a non-TCI radio* in the README.
  - ShackLog will **not** start `rigctld` for you. That program takes the serial port
    exclusively, and a second copy fighting the first is a classic cause of CAT
    dropping out mid-contest.

### Fixed
- **Switching logs did not reconnect the radio.** Radio settings live inside each log,
  so opening a different log could leave the previous log's connection in place — or
  none at all. Invisible while every log used the same TCI server; it stopped being
  true the moment a log could choose a CAT radio instead.
- The header kept the previous log's callsign after a switch.
- The connection indicator said "TCI" even when following a radio over CAT, which sent
  you to the wrong settings page to work out why a link was down. It now names
  whichever link is actually in use.

## [0.5.0] - 2026-08-05

### Added
- **Every QSO now records which radio made it.** With several radios in the
  shack, a finished log could not answer "which rig worked that station?" —
  the `station` column existed but was never written. ShackLog now takes the
  radio name from the TCI connection, shows it in the status bar while you
  work (`TCI: ✓ Hermes-Lite 2 @ 127.0.0.1:40001`), and stores it on each QSO.
  It exports as ADIF `MY_RIG` and reads back in, so it survives a round-trip.
  - **Radio nickname** (Settings → TCI) — TCI reports the *application*, not
    the radio, so AetherSDR answers "AetherSDR" whether a Hermes-Lite 2 or a
    FLEX-6700 is behind it. A nickname is the only thing that can tell two
    rigs on the same software apart, so it takes precedence over the
    announced name. It is remembered per host and port.
  - **A blank is better than a guess** — a QSO logged while TCI is
    disconnected records no radio at all, rather than inheriting whichever
    radio was connected last. Existing QSOs are left untouched.

- **The window title and About box now show the version**, and any build that
  is not an official release is marked `-dev` (e.g. `ShackLog — G0JKN —
  0.5.0-dev`). An installed copy and a freshly built one are otherwise
  indistinguishable on screen, which has cost real debugging time — a feature
  was once tested against a months-old installed binary and appeared broken.

### Changed
- The version is now taken from the build system in one place instead of being
  typed into a source file, where it had previously drifted several releases
  behind without anyone noticing.

## [0.4.1] - 2026-07-30

### Added
- **The logbook now protects itself.** A ham's log is the one file in the
  shack that cannot be remade, so ShackLog now treats it that way:
  - **Verified backups** — a compacted snapshot (`VACUUM INTO`, safe against a
    live database, unlike a file copy) written automatically at most weekly and
    **before every schema migration**, each one verified by reopening it and
    checking it before it counts. The newest five automatic and three
    pre-migration snapshots are kept in a `backups/` folder beside the log.
  - **Integrity check on open** — a fast page check every time, the full check
    only if that complains.
  - **Quarantine and restore** — a logbook that fails its integrity check has
    its damaged files moved aside (never deleted) and the newest verified
    backup restored in their place, with a dialog telling you exactly what
    happened and which QSOs might be missing. With no backup available,
    ShackLog keeps logging on the damaged file rather than locking you out
    mid-contest — but says so loudly.

### Fixed
- **Automatic backups could never have run**: several `PRAGMA` statements
  return a row, so the settings query in `open()` stayed active for the whole
  function and `VACUUM INTO` refused to run behind it ("SQL statements in
  progress"). Found by the new durability tests before any release shipped.

### Added (also new since 0.4.0)
- **Grid Map** (Maps → Grid Map) — a Maidenhead grid tracker: equirectangular
  world with the AA–RR field lattice, every 4-character square worked painted
  at its true location (dim green worked, bright green confirmed — LoTW or
  QSL card, the same rule the Awards panel uses), band/mode filters, hover
  details, live repaint as QSOs are logged. The continent backdrop is
  deliberately coarse hand-laid geometry in the Section Map spirit.
  Clicking a worked square filters the main log to it (via the ordinary
  filter box, so clearing works the usual way); clicking again clears.

### Fixed
- LoTW dialog window title rendered a literal `&&`.

## [0.4.0] - 2026-07-30

### Added
- **LoTW sign & upload + confirmations** (Tools → LoTW Upload) — exports the
  QSOs that have never been uploaded (or a chosen date range) and hands them
  to ARRL TrustedQSL (`tqsl`) for signing and upload; on success they are
  marked `LOTW_QSL_SENT=Y` with the upload date. tqsl is auto-detected and
  the certificate never leaves it. A second panel fetches your LoTW
  confirmations (website login, not the certificate passphrase) and applies
  them to the log as `LOTW_QSL_RCVD=Y`, which the Awards panel counts as
  confirmed. Brand-new LoTW users get a guided setup banner instead of an
  error when tqsl isn't installed yet.
  Schema v3 adds `LOTW_QSLSDATE`/`LOTW_QSLRDATE` (both round-trip through
  ADIF export/import). Older ShackLog builds cannot open a migrated log —
  update every machine that shares one.

### Fixed
- The startup operator chooser could open hidden behind other windows,
  making launches look broken (and once left an old installed copy driving
  the session unnoticed). It now stays on top and takes focus.
- **APRS Activity window** (Tools → APRS Activity) — connects to AetherSDR's
  KISS-over-TCP TNC (default `127.0.0.1:8001`), decodes the off-air AX.25/APRS
  traffic, and shows a live roster of heard stations with great-circle
  distance/bearing from `MY_GRIDSQUARE`, time-since-heard, APRS symbol, and a ✓
  against any call already in the log. Stale stations age out (default 1 h).
  A message row sends APRS text back out through AetherSDR. The decoder
  (`AprsDecode`) is a clean-room KISS/AX.25/APRS implementation with 23 unit
  tests (build with `-DSHACKLOG_TESTS=ON`); the socket client
  (`AprsKissClient`) auto-reconnects like the TCI client.
- **Server: live ARRL Field Day score** — `GET /api/score` returns a JSON
  breakdown (QSO points × power multiplier + bonus), classifying phone/CW/
  digital per FD rules; power multiplier and bonus are query params.
- **Server: N3FJP network mirror** — `N3fjpClient` connects to a remote N3FJP
  server and logs its QSOs into the local database.

### Changed
- **Unified ADIF parsing.** The server's WSJT-X UDP receiver used its own copy
  of the ADIF field parser; it now uses the shared `AdifReader` (same parser as
  File → Import ADIF). As a side benefit, WSJT-X ingest now gets band-from-freq
  derivation, the MODE:USB/LSB → SSB+SUBMODE fold, and QSL/LoTW/eQSL field
  handling it previously lacked. Verified end-to-end with a live FT8 datagram.

## [0.3.2] — 2026-06-12

### Added
- **Section map** (Maps menu) — a native, N3FJP-style ARRL/RAC section map.

## [0.3.1] — 2026-06-12

### Fixed
- Callsign-lookup XML parsing: QRZ/HamQTH responses were consumed whole by
  `readElementText()` on a container element, so the session id never parsed
  and login always "failed". Replaced with a leaf-text token walk.
- DX cluster duplicate-login ping-pong: two clients on one call-SSID kicked
  each other in a ~1 s reconnect storm. Reconnect backoff now only resets
  after sustained uptime, kicks are detected and surfaced to the operator,
  and a kicked client backs off to 30 s.

## [0.3.0] — 2026-06-12

### Added
- **Callsign lookup chain** — three-tier autofill of empty QSO fields at save:
  worked-before → offline cty.dat prefix resolver → online (QRZ / HamQTH /
  callook.info). Bundled `cty.dat` as a Qt resource; new Settings → Lookup tab.

## [0.2.0] — 2026-06-10

### Added
- **ADIF file import** (File → Import ADIF) — whole-file, single-transaction,
  deduplicated import via a shared byte-oriented ADIF reader. Proven on a real
  16k+ record HRD export.
- **Multi-operator logs** — one SQLite database per callsign, with a startup
  operator chooser and live *Switch Operator/Log*.
- **Awards panel** (Tools → Awards) — DXCC / WAS / WAC / WAZ / grids, worked vs
  confirmed, with chase lists.
- **"How far?"** button — opens a PSK Reporter map filtered to your callsign
  and current band.
- **Desktop WSJT-X ingest** — logs WSJT-X "Secondary UDP" QSOs straight into
  the open log (solo-operating mode).
- **Server (Field Day infrastructure)** — headless companion exposing an HTTP
  API (`/api/qsos`) and an N3FJP-compatible TCP endpoint, writing into the
  shared logbook with schema versioning, an audit trail, and soft-delete;
  ingests WSJT-X Secondary-UDP spots.

### Changed
- Cross-platform verified on Windows, Linux, and macOS. Build fixed on Qt 6.4
  (`QWebSocket::errorOccurred` is Qt 6.5+).

## [0.1.2] — 2026-05-08

### Added
- Initial release: SQLite-backed logbook, TCI WebSocket auto-fill of freq /
  band / mode, quick QSO entry with live duplicate check, filterable QSO
  table, full-fidelity QSO editor, ADIF 3.1.4 and Cabrillo 3.0 export, and a
  cross-platform GitHub Actions release workflow.

[Unreleased]: https://github.com/nigelfenton/shacklog/compare/v0.3.2...HEAD
[0.3.2]: https://github.com/nigelfenton/shacklog/releases/tag/v0.3.2
[0.3.1]: https://github.com/nigelfenton/shacklog/releases/tag/v0.3.1
[0.3.0]: https://github.com/nigelfenton/shacklog/releases/tag/v0.3.0
[0.2.0]: https://github.com/nigelfenton/shacklog/releases/tag/v0.2.0
[0.1.2]: https://github.com/nigelfenton/shacklog/releases/tag/v0.1.2
