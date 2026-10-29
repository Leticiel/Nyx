# Nyx

Modding tool for the Cyberworks (C,system) game engine.

- **Partial** — Configuration and scenarios.
- **Full** — Configuration, scenarios and media.

## Usage

1. Open `Nyx.exe`.
2. Choose a mode, click **Decompile game**, and select the game folder.
3. Edit the exported files in `.project`.
4. Click **Compile project** and select `.project`. Rebuilt files are saved to `.build` in the game folder.

Existing output folders are replaced when decompiling or compiling again.

## Tested

- [Yumekoi Tensei](https://vndb.org/v1036)
- [Doki Doki Oyako Lesson](https://vndb.org/v1434)
- [Natsu Doki! Harem](https://vndb.org/v2192)
- [Gakkou Yarashii Kaidan](https://vndb.org/v2456)
- [Aniyome wa Ijippari](https://vndb.org/v2480)
- [Gibo no Toiki](https://vndb.org/v2495)
- [Iji Care](https://vndb.org/v3077)
- [Harami no Tane](https://vndb.org/v3317)
- [Aneboku](https://vndb.org/v3887)
- [Kairaku Izonshou](https://vndb.org/v4315)
- [Oshioki](https://vndb.org/v6331)
- [Aru Kazoku no Kankeizu](https://vndb.org/v9977)
- [Zettai Majiiki](https://vndb.org/v10567)
- [Roshutsu Hentai Yuugi](https://vndb.org/v11227)
- [Fuka no Jugoku](https://vndb.org/v13835)
- [Shukubo no Uzuki 2](https://vndb.org/v17264)
- [Grand Guignol no Yoru](https://vndb.org/v22042)
- [Tsuma no Haha](https://vndb.org/v32745)
- [Mugen no Sakura 2](https://vndb.org/v35029)
- [Akuma to Yoru to Isekai to](https://vndb.org/v37725)
- [Tomoe no Tantei Monogatari](https://vndb.org/v60306)
- [Hitozuma Joushi no Wakarasekata](https://vndb.org/v61333)
- [Mujina Shuu](https://vndb.org/v61648)

## Build

Requires Windows, CMake 3.20 or newer, and a C++20 compiler (MSVC or MinGW).

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```
