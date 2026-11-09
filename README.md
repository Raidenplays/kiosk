# Kiosk

A store-style front end for [Universal-DB](https://db.universal-team.net) on 3DS.

Kiosk keeps Universal-Updater's download/install engine and replaces the whole interface:

- **Home** — shelves (Recently updated, Popular, and one per category). The top screen previews whatever's focused.
- **Search** — tap the field or press Y.
- **Updates** — its own tab with a badge. A updates one app, X updates everything, Y opens the app page.
- **Downloads** — progress ring on the top screen; X cancels the running item, Y removes a waiting one. Prompts from install scripts show up here.
- **Library** — everything you've installed.
- **App page** — pick a download, X for release notes (L/R scrolls), Y for screenshots (L/R flips).

L/R switch tabs, B goes back, START quits.

Kiosk shares `sdmc:/3ds/Universal-Updater/` (stores, MetaData.json, config) with Universal-Updater, so anything you installed with either app shows correctly in both.

## Building

`make` with devkitARM (needs 3ds-curl, 3ds-libarchive, 3ds-mbedtls, citro2d). Push to GitHub and the Actions workflow builds `Kiosk.3dsx` and `Kiosk.cia` for you.

Based on Universal-Updater by Universal-Team (GPLv3).
