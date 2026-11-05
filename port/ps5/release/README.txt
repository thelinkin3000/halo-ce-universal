SlipstreamCE, Halo: Combat Evolved for the PS5 (PPSA99500)

What the console needs: system software 13.60 or lower, jailbroken (the
Relapse exploit, or another kernel exploit, then kstuff); ShadowMountPlus
1.7beta5 or later, which puts homebrew in /data/homebrew on the home screen;
and an FTP server such as ftpsrv. It was tested with etaHEN loaded as well,
which it does not need. You also need your own copy of the game's Xbox disc
image (.iso or .xiso).

Installing, over FTP from your computer:
1. Copy the PPSA99500 folder from this zip into /data/homebrew.
2. Make the folder /data/halo and copy the disc image into it.
3. Start SlipstreamCE from the home screen.

The first start takes the maps out of the disc image (about 1.8 GB, under
a minute); notifications show how far it has got, then the game starts.

Settings are in the game's menus, and in /data/halo/config.toml, which you
can edit over FTP. Saves, the log (halo.log) and screenshots are there too.
Voice chat talks while the V key is held: to talk with the pad, put
push_to_talk = "Gamepad LB" under [controls] in config.toml.

Updates: the game looks for a new release when it starts and installs it
while you play; when a notification says so, close the game and start it
again. To stop it, put update.auto = false in config.toml. To update by
hand, copy a newer release's PPSA99500 folder over the old one.

Everything else (what it does, the network, building it):
https://github.com/thelinkin3000/SlipstreamCE/blob/main/port/ps5/README.md
The licences are in COPYING and licenses/; NOTICE.txt says where the source is.
