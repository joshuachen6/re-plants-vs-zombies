# re-plants-vs-zombies fork

Fork of Patoke's PVZ GOTY decompilation with the following objectives:
- [x] Replace renderer with SDL + OpenGL
- [x] Replace Windows code with cross-platform code
- [x] Replace DirectSound/BASS/FMod with [SDL Mixer X](https://github.com/WohlSoft/SDL-Mixer-X)
  * This project uses a fork of SDL Mixer X that adds compatibility with the MO3 format by using libopenmpt. This fork is located under SexyAppFramework/sound/SDL-Mixer-X
- [x] main.pak support
- [x] Optimize memory usage for console ports (Partial)
* Port the game to these platforms:

| Platform        | Data path                    | Status                                                                                 |
|-----------------|------------------------------|----------------------------------------------------------------------------------------|
| Windows (SDL2)  | Same as executable           | Works                                                                                  |
| Linux (SDL2)    | Same as executable           | Works                                                                                  |
| Haiku (SDL2)    | Same as executable           | Partially works: no music                                                              |
| Nintendo Switch | sdmc:/switch/PlantsvsZombies | Works on real hardware and Citron. Kenji-NX crashes on boot.                           |
| Nintendo 3DS    | sdmc:/3ds/PlantsvsZombies    | In development, might not have enough memory for Old 3DS, might barely work on New 3DS |
| Nintendo Wii U  |                              | No work started yet, but planned                                                       |
| Web (wasm)      | Bundled, or chosen in-page   | Works in Chromium; see [Building for the web](#building-for-the-web)                   |

To play the game, you need the game data from PvZ GOTY. Copy "main.pak" and the "properties" folder to the path where the re-plants-vs-zombies executable is (or one of the above data paths for console ports).

Alternatively, you could use a PvZ .pak extractor tool and use extracted data instead of main.pak, but that's up to you

## Building for the web

The game can be compiled to WebAssembly with [Emscripten](https://emscripten.org/docs/getting_started/downloads.html). SDL2, zlib, libpng and libjpeg come from Emscripten's ports, and libopenmpt is downloaded and built automatically, so only emsdk, CMake, Ninja and make are needed:

```
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -G Ninja -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web
```

This produces `re-plants-vs-zombies.html`, `.js` and `.wasm` in `build-web`. Serve that folder over HTTP (for example `python3 -m http.server -d build-web`) and open the .html file. The page asks the player to choose their PvZ GOTY folder; the files are only loaded into the tab, not uploaded.

To bundle the game data into the build instead, point `PVZ_WEB_DATA_DIR` at a folder containing `main.pak` and `properties`. This adds a `re-plants-vs-zombies.data` file that the page downloads at startup:

```
emcmake cmake -G Ninja -B build-web -DCMAKE_BUILD_TYPE=Release -DPVZ_WEB_DATA_DIR=/path/to/pvz-data
```

### Versus mode (multiplayer)

The web build adds a **Versus** button to the main menu: a head-to-head mode in the style of Bloons TD Battles. Each player defends their own lawn, and the only zombies are the ones your opponent sends you.

- One player clicks Versus, picks a map (Day, Night, Pool, Fog or Roof) and leaves the code empty to make a room; the other enters the 4-letter room code.
- Both pick 6 plants (every plant is available), then 4 zombies from the 15 that can be sent. The zombie chooser shows each zombie's cost, income bonus, cooldown and the wave it unlocks at, and whether your opponent is ready. On the roof, Diggers, Dancers and Zombonis can't be picked.
- The match starts for both players at the same moment, once both have finished choosing.
- Your zombie packets sit on the right of the seed bank. Pick one up and click a lane in the opponent view (top right), or a row of your own lawn, to send that zombie down the same lane of your opponent's lawn.
- There is no falling sun. Instead you get income every 6 seconds, and every zombie you send raises your income, so sending zombies is also how you grow your economy. Sunflowers still work.
- Sent zombies cost about what the plants to stop them cost (a Normal zombie is 100 sun, a Buckethead 300, a Gargantuar 750), and tougher ones unlock as the natural waves go by: nothing can be sent before wave 1, Football Zombies from wave 7, Gargantuars from wave 11. Each send raises your income by up to an eighth of its cost, which pays back about as fast as a sunflower; the raise shrinks as your income approaches its ceiling of 250 sun per payout, so late-game sun doesn't run away.
- From wave 10 each send is a pack of 2 zombies at twice the price, then 3 from wave 18 and 4 from wave 26, so there's always something to spend sun on and the pressure keeps rising.
- A zombie sent down a lane it can't use (a Football Zombie into the pool) walks down the nearest lane it can.
- Like the rounds in Bloons TD Battles, both lawns also get the same natural waves at the same time: the first 25 seconds in, then every 25 seconds, growing and adding tougher zombies as the match goes on. Every 5th wave is a huge wave.
- The top-right view shows your opponent's lawn, sun and income, updated 10 times a second.
- The first player whose lawn a zombie gets through loses. Lawnmowers work as usual. Leaving the match forfeits it.
- If a player's connection drops, both games pause while that game reconnects by itself. The server holds the player's spot for 60 seconds and nothing sent in the meantime is lost; after that, the opponent wins. A match also keeps running if its browser tab is in the background.

The game talks to a small relay server in `server/` (Node.js), which pairs players by room code and forwards their messages:

```
cd server && npm install
node relay.js --port 8787 --static ../build-web
```

With `--static` the relay also serves the game, so opening `http://localhost:8787/` in two browser windows is enough to play locally. By default the game connects to `/versus` on the server it was loaded from; add `?server=ws://host:port` to the page URL to use a different relay. `server/deploy/` has a systemd unit and an nginx snippet for running the relay behind nginx. Run `npm test` in `server/` to test the relay. Balance values (income, zombie prices and cooldowns, wave timing and growth) are constants at the top of `Lawn/System/Versus.h` and `Versus.cpp`.

Profiles and saved games are stored in the browser's IndexedDB. The web port lives in `SexyAppFramework/platform/web` (WebGL renderer, window, input), `web/` (page shell and filesystem setup) and `CMake/Emscripten.cmake`. Unlike the other platforms, the main loop runs under Asyncify so it can hand control back to the browser every frame, and resources load on the main thread instead of a loading thread.

Original README continues below

# re-plants-vs-zombies

A project focused on decompiling the latest functionality from the first PvZ title and expand upon the game and its engine

The SexyAppFramework dating as back as 2005 is a very old game engine and it does not follow proper C++ conventions as per modern standards nor does it use a modern renderer backend

This project aims to modernize the engine by using features from the latest C++ standards aswell as replacing the old legacy DirectDraw and Direct3D7 renderers for the modern [GLFW](https://www.glfw.org/) cross-platform wrapper aswell as expanding upon an old (now deleted) decompilation project of PvZ version 0.9.9 by [Miya aka Kopie](https://github.com/rspforhp) to get the best possible PvZ experience both for modders and players alike

# DISCLAIMER

This project does not condone piracy

This project does not include any IP from PopCap outside of their open source game engine, this will only output the executable for a decompiled, fan version of PvZ

To play the game using this project you need to have access to the original game files by [purchasing it](https://store.steampowered.com/app/3590/Plants_vs_Zombies_GOTY_Edition/)

## Roadmap

#### Currently focused on
- [x] Add x64 support for the base game **(Partial)**
- [ ] Replace the old renderer backend for GLFW **(WIP)**
- [ ] Replace all Windows only code for cross-platform GLFW counterparts **(WIP)**

#### Left for when we have a working x64 build using GLFW
- [ ] Add all functionality from the GOTY version of the game
  - [x] Achievements **(Partial)**
  - [ ] Zombatar

#### Possible future features
- [ ] Create an easy to use modding API for the game
  - [ ] Parse zombies from files
  - [ ] Parse plants from files
  - [ ] Parse maps from files
  - [ ] Add scripting for custom sequences

## Installation

### Visual Studio Community

Open the folder containing the `CMakeSettings.json`, wait until cache finishes generating and build the project

### Other (Sublime, Visual Studio Code, MSYS2, etc..)

Run the following commands (assuming you have CMake installed with Ninja) where the `CMakeSettings.json` file is located

`cmake -G Ninja -B cmake-build`

`cmake --build cmake-build`

If running these commands does not create a successful build please [create an issue](https://github.com/Patoke/re-plants-vs-zombies/issue) and detail your problem

After you build, the output executable should be in the `Debug` or `Release` (depending on your build target) folder inside `SexyAppFramework`

Then you want to copy that executable inside of the original game's root folder (or copy the contents of the original game folder inside the previously mentioned folder)

After that you should be able to just open the built executable and enjoy re-pvz!

## Contributing

When contributing please follow the following guides:

<details><summary>SexyAppFramework coding philosophy</summary>

#### From the SexyAppFramework docs:

<br>
The framework differs from many other APIs in that some class properties are not wrapped in accessor methods, but rather are made to be accessed directly through public member data.   The window caption of your application, for example, is set by assigning a value to the std::string mTitle in the application object before the application’s window is created.  We felt that in many cases this reduced the code required to implement a class.  Also of note is the prefix notation used on variables: “m” denotes a class member, “the” denotes a parameter passed to a method or function, and “a” denotes a local variable.
</br>
</details>

<details><summary>Contributor markings</summary>

<br>
Whenever you need to leave a comment for other developers to find you should do so with the following grammar:

* Always include the name of the contributor as in:
  * `@Contributor`
* For todos include the todo marking as in:
  * `@Contributor todo`
* Always add a colon to specify that the start of the comment starts there
  * `@Contributor todo: Thing went wrong!`
* If a new function has been reversed and you have found the address in the latest version of the game (or have reversed a certain class member offset) please note it as follows:
  * `@Contributor GOTY: 0xADDRESS`
</br>
</details>


## Thanks to

- [@rspforhp](https://www.github.com/octokatherine) for their amazing work decompiling the 0.9.9 version of PvZ
- [@ruslan831](https://github.com/ruslan831) for archiving the [0.9.9 decompilation of PvZ](https://github.com/ruslan831/PlantsVsZombies-decompilation)
- The GLFW team for their amazing work
- PopCap for creating the amazing PvZ franchise (and making their game engine public)
- All the contributors which have worked or are actively working in this amazing project
