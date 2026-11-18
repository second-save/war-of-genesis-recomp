THE WAR OF GENESIS (창세기전, 1995) - native port by static recompilation
https://github.com/ACR-GD/war-of-genesis-recomp

An unofficial, fan-made native version of Softmax's DOS strategy RPG, with an
English translation. It runs the original game's code, translated to run
directly on your computer, with no emulator.

THIS DOWNLOAD CONTAINS NO GAME FILES. You need the files of the DOS game
(Korean release, 1996): a folder with MAIN.EXE, CHRSYS.EXE, TTL.EXE, CHP.EXE,
PACK.EXE and the CHAPDATA, CSTLDATA, SCENARIO... folders.


STARTING THE GAME
-----------------
macOS:  unzip, then move "War of Genesis.app" to Applications (optional).
        The first time, right-click (or Control-click) the app and choose Open,
        then Open again. If macOS says the app "is damaged" or can't be checked,
        open System Settings > Privacy & Security and click "Open Anyway".
        Apple silicon and Intel Macs, macOS 11 or later.

On first launch the game asks for your game folder (or finds it by itself if
it is next to the app, named The-War-of-Genesis_DOS_KO). The files are copied
once to your user folder; your own folder is never changed. The game runs and
saves there:
  macOS:   ~/Library/Application Support/Softmax/WarOfGenesis/game
  Linux:   ~/.local/share/Softmax/WarOfGenesis/game
  Windows: %APPDATA%\Softmax\WarOfGenesis\game
Delete that game folder to copy your files again (this also deletes saves).


CONTROLS
--------
Mouse, as in the original. The game draws its own pointer.
F12            keyboard pointer on/off: arrows or numpad move the pointer
               (Shift = faster), Return/Z = left click, X/Backspace = right click
Space          skip story text
F11, Alt+Enter fullscreen
Cmd+Q, Alt+F4  quit
Keys, volumes and the language are in settings.ini, in the same folder as game/.


BATTLES
-------
The original manual is lost, so this was worked out from the game's own code.

Before the fight
  Click through the briefing (the mission objective).
  The roster appears: click a name to go to the map. Your units stand on the
  blue squares; click a unit to pick it up, click a blue square to put it
  down. Right-click opens the roster again; right-click on the roster asks
  "Finish deployment?": left-click to confirm (right-click to go back).

Your turn ("Player Phase")
  Every unit acts once per turn and has a stock of TIME: the green dial at
  the top left while a unit is acting. Each command costs time; a unit can
  give several commands until its time runs out. The turn ends by itself
  when every unit has acted.

  Right-click            open the command ring of the next unit to act
  In the ring:
    click the unit       MOVE: the reachable area turns grey, click where to
    (centre of the ring) go; each step costs time
    click an icon        give that command (list below)
    right-click / Esc    done with this unit (unused time is spent resting,
                         which restores some HP)
  Left-click a unit      show it on the map (selection box and HP)
  Click near a screen    scroll the map (arrow keys also scroll)
  edge
  Space                  end your turn now
  Esc                    options menu (right-click to close)
  F1                     skip battle animations on/off ("Screen Skip")
  F2 / F3                music / sound effects on/off

  Ring icons (only the ones the unit can use, and only with enough time):
    small sword          attack an enemy in range (needs 50 time)
    crossed weapons      special technique for the weapon type (sword, spear,
    (sword/spear/axe/    axe, bow): opens a second ring of techniques; needs
    bow)                 experienced units. Units learn them in this order
                         (time cost in brackets); the original icons were
                         single Chinese characters, shown here in English:
                           sword  Combo (80), Bind (80), Counter (60),
                                  Fly (100), Kill (70), Soul (150), Break (200)
                           spear  Bind (70), Combo (100), Fly (150), Soul (120)
                           axe    Counter (60), Kill (60), Fly (70), Rage (120)
                           bow    Kill (70), Combo (90), Bolt (120)
    scroll               magic: pick a spell from the list (needs MP)
    heart                rest: spend the remaining time to recover HP
    flag                 rally: raises the army's MORALE (top bar); only as
                         the unit's first action
  Not yet confirmed (named from the pictures and the code):
    blue shield          defensive stance (40 time)
    knight with shield   guard stance (60 time)
    flame                hold: ends the unit's action and keeps its time;
                         only as the unit's first action
    running figure       same as left-clicking the unit
    wings, gem, potion,  skills of particular classes
    beast, blue orb

  MORALE (top bar, blue = you, red = enemy) affects how the battle goes;
  the mission's win/lose conditions are in the options menu (Esc > Mission).


TRANSLATION
-----------
Everything the game shows as text is in English: story, town talk, battle
briefings and dialogue, names, items, magic, menus, the title menu and the
picture text (chapter titles, character pages, story scroll, credits, the
battle technique icons). Only the logos stay in the original. Set
"language = ko" in settings.ini to play the original Korean.


The War of Genesis is (c) Softmax. This project contains only original code
and code generated from it, and is not a substitute for the game.
