# Safety And Scope

This repository is for local research, modding documentation, debugging, and owned-project analysis.

## In Scope

- Read-only module, section, string, RTTI, import, export, and xref discovery
- SDK-style header and JSON generation from local data
- Candidate reports for engine systems such as rendering, time, physics, entity, and environment systems
- IDA, Ghidra, Binary Ninja, and ReClass note exports for offline analysis
- Local folder scans of files you are allowed to inspect
- Runtime diagnostics in an owned process or project you control
- Project bridge exports that your own application intentionally exposes

## Out Of Scope

Do not add or request:

- Anti-cheat bypasses
- Stealth, evasion, or anti-debug bypass logic
- Protected-file decryption, unpacking, or tampering
- Networked cheating features
- Instructions for unauthorized use in third-party online games
- Patches that modify a third-party process without permission
- Secrets, tokens, or proprietary game dumps committed to Git

## Read-Only SDK Dumper Boundary

The SDK generator should collect evidence and generate reports without writing to the target process. Its useful outputs are:

- Headers
- JSON files
- Markdown reports
- Candidate traces
- Label scripts
- SQLite exports

If a provider, registry, reflection system, or CVar table is not found, reports should say that honestly and list candidates instead of claiming confirmed data.

## Runtime DLL Boundary

`FrostbiteUniversal.dll` is intended for diagnostics in an owned process or a project where you have permission to load it. Keep public documentation focused on normal local plugin, test harness, and owned-project workflows.

The bridge template's timescale, skybox/fog tint, debug material tint, actor/model table, model boxes, snaplines, FOV, and view-angle preview examples are owned-project/editor diagnostics. Treat them as integration examples for projects you control, not as instructions for unauthorized online play or third-party process modification.
