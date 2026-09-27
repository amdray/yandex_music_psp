# YMPSP

Русская версия: [README_RU.md](README_RU.md)

An unofficial Yandex Music client for PlayStation Portable.

YMPSP is not affiliated with or endorsed by Yandex LLC.

## Features

- Direct connection to Yandex Music from the PSP
- TLS 1.2 via mbedTLS
- PSP KIRK PRNG used as the entropy source for TLS
- No external proxy or companion server
- Playlists and albums
- Search
- Music streaming
- Local caching
- Album artwork
- NTP time synchronization
- MP3 playback up to 320 kbps using the PSP's native `sceMp3` decoder

## Screenshots

![Main menu](screenshots/menu.jpg)

![Playlists](screenshots/playlists.jpg)

![Now playing](screenshots/player.jpg)

## First Launch

There are two ways to sign in:

**Using a device code.** Open **Profile** from the menu (or press `X` on the empty account screen), enter the displayed code at `ya.ru/device`, and confirm the login. The token will be saved automatically to `config/token.txt`.

**Manually (recommended).** Open the empty `config/token.txt` file in the built or extracted application directory and enter your token between the quotes:

```text
YANDEX_TOKEN = "..."
```

Then:

1. Copy the application directory to `PSP/GAME/YMPSP/` on the Memory Stick.
2. Make sure `assets` and `fonts` are located next to `EBOOT.PBP`. Runtime directories inside `data` are created automatically.
3. Enable WLAN and configure a working Wi-Fi connection in the PSP system menu.

A populated `config/token.txt` containing a real token must not be committed to Git or included in public archives. The empty template `YANDEX_TOKEN = ""` created by the build does not contain any secret data.

## Current Status

The application targets PSP systems running ARK-4 or ARK-5 and is under active development.

## Building

YMPSP is built using PSPSDK.

From the project root:

On Linux:

```
make
```

In PowerShell:

```
wsl --shell-type login make
```

The result is created at `release/EBOOT.PBP`.

The `make` command also copies tracked resources from `fonts/` and `assets/` into the corresponding directories under `release/` and, if missing, creates an empty `release/config/token.txt` containing:

```text
YANDEX_TOKEN = ""
```

You can populate the token file manually before launching the application or sign in using the device-code flow directly on the PSP. Subsequent builds do not overwrite an already populated token file.

## License

The source code is distributed under the MIT License — see [LICENSE](LICENSE).

Yandex names and trademarks belong to their respective owners and are not covered by the MIT License. Album artwork shown in the screenshots also belongs to the respective copyright holders.
