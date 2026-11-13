# Security policy

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use GitHub's private reporting instead: open the [Security tab](../../security), choose "Report a vulnerability" and describe the problem, with steps to reproduce if you can. The report stays private until a fix is out.

Reports are answered as soon as possible, usually within a few days. This is a hobby project, so there is no fixed deadline.

## Supported versions

Only the latest release gets fixes. Please check that the problem is still there in the latest release before reporting.

## Scope

In scope:

- the mod DLL (`d3d9.dll` proxy, ModLoader build) and the setup: files written outside the game folder or `Documents\TrackMania\TMVS\`, anything that lets a crafted preset, LUT, capture or settings file run code
- the ModLoader update server and release downloads: the SHA-256 listed for each release has to match the file
- the video export and clips: files written outside the chosen folder

Out of scope:

- vulnerabilities in the games, the Competition Patch or other mods loaded next to it
- GPU driver crashes with unusual settings (report these as normal bugs)
- problems that need an attacker who already controls your PC
