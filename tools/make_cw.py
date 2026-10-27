"""Build the A.I.M. 2 Clan Wars (GOG) co-op package: dist/ClanWars-Coop/ and .zip.

The folder's contents are pasted into the GOG install (D:\\GOG Galaxy\\Games\\AIM 2,
next to AIMII.exe); no game file is replaced (the menu atlas with the
MULTIPLAYER button is served from Data\\Lang\\en by the loader).
"""
import os, shutil, zipfile

DEV = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OUT = os.path.join(DEV, 'dist', 'ClanWars-Coop')

INI = """; A.I.M. 2 Clan Wars co-op mod settings (all of them are also in the main menu > MOD SETTINGS)
[General]
Language=en

[Video]
; fullscreen, windowed or borderless
DisplayMode=fullscreen
; 0 = skip the intro videos at startup
IntroVideos=1
; renderer: 8 = the game's original Direct3D 8, 9 = Direct3D 9 (needed by the improved graphics)
Renderer=8
; shadow texture resolution with Direct3D 9: 1 (original), 2 or 4
ShadowScale=1
; how far gliders cast shadows: 0 original, 1 far, 2 maximum
ShadowDistance=0
; anti-aliasing: 0 (off), 2, 4 or 8 samples
Antialiasing=0
; vertical sync in fullscreen: 1 = on (no tearing), 0 = off, -1 = the game's setting
VSync=-1
; forced anisotropic texture filtering: 0 = the game's filtering, 2 - 16
Anisotropy=0
; Direct3D 9 renderer: explosions, missiles and shots light up their surroundings (1 = on)
DynamicLights=1
; frame rate limit: 0 = off, -1 = the monitor's refresh rate, or frames per second
FrameLimit=0
; 1 = use the game folder's own d3d8.dll (GOG's DirectX wrapper, settings in dxcfg.ini);
; 0 = Windows' Direct3D 8, so the display modes above apply
GameWrapper=0

[Coop]
; your name in co-op games (also MULTIPLAYER > Your name); empty = your pilot's name from
; the game (the one given in the opening dialogue), else the computer name
Name=
; 1 = show other players' names above their gliders
ShowNames=1
; radar / minimap colour of other players of your clan (AARRGGBB, allies are FF00FF00)
PlayerColor=FFFFFF00

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

[Dev]
; tilde menu > 4. DEBUG: teleport to buildings / players, enter buildings, unlock passages
DebugMenu=1

[Audio]
; 1 = sounds keep playing when the game window is in the background (also avoids a
; DirectSound freeze when entering bases)
GlobalFocus=1

[Economy]
; Smart supply: buildings get their input goods from producers' surplus every
; game hour, so production efficiency and clan economies don't collapse
AutoSupply=1
; share of a producer's surplus that can be moved per hour (0.1 - 1)
SupplyRate=0.5
; stock (in value units) a producer always keeps for convoys and the player
ProducerReserve=2000
"""

README = """A.I.M. 2 Clan Wars co-op (preview)
==================================

For the GOG version of A.I.M. 2 Clan Wars (1.4.30 / 1.4.31).

Install: copy everything in this folder into your game folder (GOG: "AIM 2",
next to AIMII.exe). No game file is replaced. To uninstall, delete
dinput8.dll, ClanWarsCoop.ini, ClanWarsCoop.log, ClanWarsCoop-LICENSE.txt and the
Data\\Lang folder.

License: PolyForm Noncommercial 1.0.0 (ClanWarsCoop-LICENSE.txt) - free for any
noncommercial use; keep the license with every copy you share.

Playing together (LAN):
  * Main menu > MULTIPLAYER opens the co-op list. "Your name" (first entry):
    select it, press the button and type the name other players see.
  * "Host a new game" starts a new campaign that others can join;
    "Host: <save>" continues a saved game as a co-op game.
  * Games hosted on your network appear as "Join: <name>"; select one and
    press the button. You receive the host's world and start next to the
    host, flying your own glider (your pilot is kept in
    SAVES\\coop_<host>_<your name>.bin, one per host, so you continue with the
    same glider and equipment next time). "Join: <name> - own clan" keeps your own
    missions and log.
  * Allow AIMII.exe through the Windows firewall (TCP/UDP port 28620).

Shared: the world (the host's NPCs, their shots and deaths, the time of
day), the quest log and notices (who joined, who entered which base).

Settings: SETTINGS in the main menu is the game's own settings dialog, unchanged.
MOD SETTINGS (below it) opens the same dialog with the mod's options, one list per
option; USE applies them, CLOSE discards changes. "(restart)" options apply the next
time the game starts. The options:
  * Display mode (fullscreen / windowed / borderless, switches live), Renderer
    (Direct3D 8 = the original, Direct3D 9 = the mod's renderer; the window title
    says "Direct3D 9" when it runs), Shadow resolution and distance,
    Anti-aliasing, VSync, Anisotropic filtering, Gamepad, Background
    sound, Save anywhere (F5/F9 outside bases), Intro videos, Show player names,
    Debug menu, Frame rate limit.
  * Dynamic lighting (Direct3D 9 renderer): explosions, missiles and shots light up the
    ground, buildings and gliders around them, per pixel.
  * Smart supply (economy fix): the game's trade convoys pick random routes
    and cargo, so buildings run out of the goods they need, their
    production efficiency drops, clan income collapses, clans stop buying
    gliders and end up abandoning their buildings. With Smart supply on,
    every game hour each building's low inputs are refilled from the
    surplus of the buildings that produce them (producers keep a reserve,
    so convoys and you still have goods to trade). Clans stay solvent and
    expand through the economy instead of only by fighting.

CHAT: in a co-op game press T in flight, type, Enter sends the line to everybody
(Esc cancels). The key can be changed in the ini: [Coop] ChatKey = DirectInput scan
code (0x14 = T).

GAMEPAD PLAY (XInput pads, setting "Gamepad"): in flight the left stick accelerates /
brakes and strafes, the right stick turns and aims; RB fire, LB second weapon,
X rocket, A jump, B and LT brake, RT afterburner, Y target, BACK map, START menu,
d-pad ammo / night light / journal / hold. The buttons press the game's own key
bindings; change them in the ini, [Gamepad]. In menus and bases the left stick moves
the cursor (right stick faster), A = left click, B = right click, START = Esc.

Debug menu (preview): press tilde in flight, then 4. DEBUG - teleport to the
next/previous building, enter the nearest building, show your position,
teleport to another player, unlock all passages (sector tunnels). Turn it off
with [Dev] DebugMenu=0 in ClanWarsCoop.ini.

Commands to other players: target another player (TAB) and use tilde >
3. Target commands - they get a notice (e.g. "Host asks you to join their group").

Known limitations of this preview: dialogues and trading in bases are local
to each player. The mod uses Windows' Direct3D 8 instead of the GOG DirectX
wrapper (d3d8.dll) in the game folder; set [Video] GameWrapper=1 in ClanWarsCoop.ini
to use the wrapper again (its settings: dxcfg.exe).
"""


def main():
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    lang = os.path.join(OUT, 'Data', 'Lang', 'en')
    os.makedirs(os.path.join(lang, 'TM'))
    shutil.copy2(os.path.join(DEV, 'build', 'aim2cw', 'dinput8.dll'), OUT)
    shutil.copy2(os.path.join(DEV, 'tr', 'aim2cw', 'lang', 'en', 'lang.ini'), lang)
    shutil.copy2(os.path.join(DEV, 'tr', 'aim2cw', 'assets', 'TEX_INT_UI1.TM'), os.path.join(lang, 'TM'))
    open(os.path.join(OUT, 'ClanWarsCoop.ini'), 'w', newline='\r\n').write(INI)
    open(os.path.join(OUT, 'README.txt'), 'w', newline='\r\n').write(README)
    shutil.copy2(os.path.join(DEV, 'LICENSE'), os.path.join(OUT, 'ClanWarsCoop-LICENSE.txt'))
    z = OUT + '.zip'
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(OUT):
            for f in files:
                p = os.path.join(root, f)
                zf.write(p, os.path.relpath(p, OUT))
    print('wrote', OUT, 'and', z)


if __name__ == '__main__':
    main()
