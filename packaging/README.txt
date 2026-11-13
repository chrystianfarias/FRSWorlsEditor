FRS World Editor
================

A map editor for Need for Speed Underground 2, by Chrystian Farias.

Fly through the city exactly as the game ships it, then move, add and remove
objects, edit collision, import your own meshes and textures -- and launch
the game straight into free roam, with your car where the editor's camera is.


GETTING STARTED
---------------

1. Unzip this folder anywhere (not inside the game folder).
2. Run FRSWorldEditor.exe. The setup opens by itself the first time:
     - it finds the game (or you point it at the folder with SPEED2.EXE);
     - it installs the plugins the editor's work needs inside the game.
   If the game is under Program Files, Windows asks for administrator rights
   once, for the copying.
3. Pick a region in the combo box at the top left. Fly with the right mouse
   button held and W A S D (Q / E down / up, Shift faster).

Run the setup again at any time from File > Game Folder and Plugins.


WHAT THE SETUP INSTALLS
-----------------------

Into the game folder (the one with SPEED2.EXE):

    dinput8.dll                   Ultimate ASI Loader, by ThirteenAG. Skipped
                                  when the game already has an ASI loader.
    scripts\FRSWorldLoader.asi    The editor's end in the game: placed and
    scripts\FRSWorldLoader.ini    edited objects, collision, textures.
    scripts\FRSFastTest.asi       "Launch game" goes straight into free roam.
    scripts\FRSFastTest.ini       By default only when the editor launches
    scripts\FRSFastTest-Tracks.txt  the game; the setup can make it always.
                                  (It replaces FRSFastBoot: an old copy is
                                  moved to scripts\retired-FRSFastBoot.)

Older copies of the plugins (FRSModelLoader.asi, SpawnCar.asi,
FRSCameraBridge.asi) are renamed to .off, since FRSWorldLoader.asi does
their work now.

The game's own files are never changed. Everything the editor writes goes to
the "models" folder inside the game folder: delete it, and the game is back
exactly as it shipped.


GAME VERSION
------------

The plugins are written for SPEED2.EXE v1.2 (4,800,512 bytes,
MD5 6445adbf4e8d64be51e96a69bba168a4). The editor opens any version's files;
on another executable the plugins leave the game alone.

To see the game inside the editor's window, the game has to run windowed
(for example NFSU2ExtraOptions, WindowedMode). Fullscreen works too, in its
own window.


SUPPORT
-------

Questions, bugs and ideas: the official Discord, https://discord.gg/jTKwQXYQkb

FRS World Editor is free. If it helps you, you can buy me a coffee:
https://buymeacoffee.com/chrystianfarias


LICENSE
-------

FRS World Editor, FRS World Loader and FRSFastTest: (c) 2026 Chrystian Farias,
licensed under Creative Commons Attribution-NonCommercial 4.0 International
(CC BY-NC 4.0). You may share and adapt them, crediting Chrystian Farias, for
non-commercial purposes only -- they may not be sold.

Also included: the Win32 property grid by David MacDermot (CPOL) and Ultimate
ASI Loader by ThirteenAG (MIT). Full texts in LICENSE.txt.

Need for Speed and Need for Speed Underground 2 are trademarks of Electronic
Arts. This is a fan project, not affiliated with or endorsed by EA. You need
your own copy of the game.
