# Saves, upgrades and recovery

These changes are included in `v1.0.0-preview.2` and development source; older previews have different storage behavior. Keep the old HTML file until you have verified your battery backup in the new build.

## Before upgrading

1. Open each game in the old app, then use **Settings → Data → Export save**. Keep these files outside browser storage.
2. Use the same browser profile and origin for the new app. `file:` storage is browser-defined; a renamed/moved file, a different localhost port or another profile may see a different database.
3. Add the exact same cartridge to the new app. Photon64 now hashes all cartridge bytes in canonical big-endian order. Equivalent `.z64`, `.v64` and `.n64` dumps share an identity; patches and different revisions remain separate.
4. If an old cached cartridge is present and its digest matches, saves, state records, thumbnails and metadata are copied in one transaction. The original records remain recoverable. Without verified old cartridge bytes, Photon64 does not guess which save belongs to a patched cartridge: use the exported battery file explicitly.
5. Verify progress after reopening the cartridge before discarding the old app or backups.

## Battery files and states

**Export save** now writes a versioned `.p64save` envelope containing cartridge digest, core identity, medium metadata and the EEPROM/SRAM/Flash/Controller Pak payload. It contains no ROM. Import checks the cartridge digest. Old raw Photon64 battery files of exactly 264,192 bytes remain accepted; they do not contain an identity and should be selected carefully. Battery payloads are intentionally compatible across core upgrades. Medium metadata is descriptive: importing a file does not silently change the game's save hardware.

**Export state / Import state** uses `.p64state`. It requires the same canonical cartridge digest, the full WASM SHA-256 and the internal state header/size. It is useful across devices running the same core build. A different source commit with identical core WASM can remain compatible; a changed core cannot. No conversion of old machine layouts is attempted. Loading a state restores its battery bytes, drains earlier writes and marks the restored battery for a new durable commit. Prefer battery backups for upgrades.

**Save hardware** creates a durable backup before resetting. One previous snapshot is retained per cartridge. **Export previous save** downloads it. **Restore previous save** atomically restores its medium and battery and keeps the replaced progress as the new backup. A failed transaction changes neither the live machine nor the previous backup. A temporary-storage session cannot promise this durable protection and cannot change save hardware.

## Multiple tabs and temporary storage

One tab holds an exclusive Web Lock for each active cartridge. A competing tab is told to close the first game before opening it; distinct games can run independently. Library updates also use one IndexedDB transaction, avoiding stale list replacement.

If Web Locks or IndexedDB are unavailable, battery/state writes remain in memory and are labelled temporary. Export before closing. Reset and loading states preserve the obligation to save; they do not imply a successful durable write. Browser eviction still removes browser-managed records: export is the recovery route. **Storage usage** reports the browser's origin estimate, not exact Photon64-only usage.

## Reporting a failure

Use **Export diagnostics** and include the reproduction steps, game revision, browser/OS and renderer. Diagnostics include full core hash/source revision, adapter, first renderer error, scale, timing and audio counters. They exclude ROM bytes and battery/state payloads. Event-to-presentation timing measures software submission after event delivery, not physical input-to-screen latency.
