"""Build the A.I.M. co-op package: dist/AIM-Coop/ and dist/AIM-Coop.zip.

The folder's contents are pasted into the GOG install
(D:\\GOG Galaxy\\Games\\A.I.M. Artificial Intelligence Machines); no game file is
replaced (assets are served from AIMCoop\\Data by the loader).
"""
import os, shutil, zipfile

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OUT = os.path.join(DEV, 'dist', 'AIM-Coop')

INI = """; A.I.M. co-op mod settings
[Video]
; fullscreen, windowed or borderless
DisplayMode=fullscreen
; renderer: 8 = the game's original Direct3D 8, 9 = Direct3D 9 (needed by the improved graphics)
Renderer=8
; anti-aliasing: 0 (off), 2, 4 or 8 samples
Antialiasing=0
; vertical sync in fullscreen: 1 = on (no tearing), 0 = off, -1 = the game's setting
VSync=-1
; forced anisotropic texture filtering (sharper distant ground): 0 = the game's filtering, 2 - 16
Anisotropy=0
; Direct3D 9 renderer: explosions, missiles and shots light up their surroundings (1 = on)
DynamicLights=1
; frame rate limit: 0 = off, -1 = the monitor's refresh rate, or frames per second
FrameLimit=0
; 0 = skip the logo videos at startup
IntroVideos=1

[Input]
; 1 = play with an XInput gamepad (see README); in menus the sticks move the cursor
Gamepad=0
; cursor speed in menus (left stick / right stick) and aiming speed in flight
GamepadSpeed=700
GamepadFastSpeed=1800
GamepadAimSpeed=900
; 1 = right stick up looks down
GamepadInvertY=0

[Gamepad]
; flight buttons: a game action from Data\\CONFIG\\cfg.ini [INPUT] (FIRE, FIREALT, ROCKET,
; JUMP, BRAKE, RUN, SELECT, SHOWMAP, ...), ESC, or KEY:<key name>
RB=FIRE
LB=FIREALT
X=ROCKET
A=JUMP
B=BRAKE
LT=BRAKE
RT=RUN
Y=SELECT
RS=SELECT
LS=ALWAYSRUN
BACK=SHOWMAP
START=ESC
UP=CHANGEAMMO
DOWN=NIGHTLIGHTTOGGLE
LEFT=SHOWJOURNAL
RIGHT=SHOWHOLD

[Audio]
; 1 = sounds keep playing when the game window is in the background
GlobalFocus=1

[Dev]
; tilde in flight: debug menu (teleport to buildings / players, enter buildings)
DebugMenu=1

[Coop]
; your name in co-op games (also MULTIPLAYER > Your name); empty = your pilot's name from
; the game (the one given in the opening dialogue), else the computer name
Name=
; 1 = show other players' names above their gliders
ShowNames=1
; radar colour of other players (AARRGGBB, allies are FF00FF00)
PlayerColor=FFFFFF00
; 1 = when you host, the world (NPCs) keeps running for the others while you are in a base
HostWorldInBase=1

; [Text] can override any of the mod's English texts, e.g.
; [Text]
; lobby_caption=Multiplayer
"""

README = """A.I.M. co-op (preview)
======================

Install: copy everything in this folder into your A.I.M. folder (GOG:
"A.I.M. Artificial Intelligence Machines", next to AIM.exe). No game file is
replaced. To uninstall, delete dinput8.dll, AIMCoop.ini, AIMCoop.log,
AIMCoop-LICENSE.txt and the AIMCoop folder.

License: PolyForm Noncommercial 1.0.0 (AIMCoop-LICENSE.txt) - free for any
noncommercial use; keep the license with every copy you share.

Playing together (LAN):
  * Main menu > MULTIPLAYER opens the co-op list.
  * "Host a new co-op game" starts a new game (choose the difficulty) that
    others can join; "Host a co-op game from a save" hosts a saved game.
  * Games hosted on your network appear as "Join <name>'s game"; select one
    and press OK. You receive the host's world and start next to the host,
    flying your own glider (your pilot is kept in SAVES\\coop_<host>_<your name>.bin, one per host,
    so you continue with the same glider and equipment next time).
  * "Your name" at the top of the list: select it, press OK and type the name
    other players see (Enter = OK, Esc = cancel).
  * "Classic network game (arena)" is the original network mode.
  * Allow AIM.exe through the Windows firewall (TCP/UDP port 28620).

Shared: the world (the host's NPCs, their shots and deaths, the time of
day) and the quest log - quests, journal entries and quest results that any
player gets are added for everyone.

Settings: the main menu's OPTIONS button opens the SETTINGS list. Click an option to switch
it to its next value (it applies at once, or the next time the game starts where the
description says "(restart)"); OK closes the list. The first entry starts the game's
original options tool. With Renderer = Direct3D 9 the window title says "Direct3D 9", and Dynamic lighting makes
explosions, missiles and shots light up the ground, buildings and gliders around them.
All settings are also in AIMCoop.ini - DisplayMode (fullscreen / windowed / borderless),
intro videos, your co-op name, the gamepad (see below) and the debug menu
(tilde in flight: teleport to buildings / players, enter buildings).

CHAT: in a co-op game press T in flight, type, Enter sends the line to everybody
(Esc cancels). The key can be changed in the ini: [Coop] ChatKey = DirectInput scan
code (0x14 = T).

GAMEPAD PLAY (XInput pads, setting "Gamepad"): in flight the left stick accelerates /
brakes and strafes, the right stick turns and aims; RB fire, LB second weapon,
X rocket, A jump, B and LT brake, RT afterburner, Y target, BACK map, START menu,
d-pad ammo / night light / journal / hold. The buttons press the game's own key
bindings; change them in the ini, [Gamepad]. In menus and bases the left stick moves
the cursor (right stick faster), A = left click, B = right click, START = Esc.

If the game crashes, AIMCoop.log contains "CRASH:" lines - please send it.

Known limitations of this preview: dialogues and trading in bases are local
to each player.
"""


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, 'AIMCoop', 'Data', 'Surfaces'))
    shutil.copy2(os.path.join(DEV, 'build', 'aim1', 'dinput8.dll'), OUT)
    for f in ('SUF_MAINMENU_BUTTONS.dds', 'SUF_MAINMENU_BUTSHL.dds', 'SUF_CAPTIONS.dds'):
        shutil.copy2(os.path.join(DEV, 'tr', 'aim1', 'assets', f), os.path.join(OUT, 'AIMCoop', 'Data', 'Surfaces'))
    open(os.path.join(OUT, 'AIMCoop.ini'), 'w', newline='\r\n').write(INI)
    open(os.path.join(OUT, 'README.txt'), 'w', newline='\r\n').write(README)
    shutil.copy2(os.path.join(DEV, 'LICENSE'), os.path.join(OUT, 'AIMCoop-LICENSE.txt'))
    z = OUT + '.zip'
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(OUT):
            for f in files:
                p = os.path.join(root, f)
                zf.write(p, os.path.relpath(p, OUT))
    print('wrote', OUT, 'and', z)


if __name__ == '__main__':
    main()
