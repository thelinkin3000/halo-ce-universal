# Settings

The game's settings are in `config.toml`, in its data folder:

| Platform | File |
| --- | --- |
| Android | `/sdcard/Android/data/ar.com.slipstreamce/files/config.toml` |
| Nintendo Switch | `sdmc:/switch/halo/config.toml` |
| PlayStation 5 | `/data/halo/config.toml` |

At the first start, the game writes the file with the default values and a
comment for each setting. To get the default values again, delete the file.

The game reads the file at start-up. If a key is not correct, or a value has
the wrong type, the game writes the line to the log and uses the default value.
A file that cannot be parsed leaves every setting at its default. A word value
needs its quotes (`menus = "pc"`, not `menus = pc`). The Settings menu changes
the most useful settings and writes them into the file for you.

Each profile keeps its own gyroscope settings (`input.gyro_aim` and the four
after it), in `settings.toml` in its folder, beside its `blam.sav`. A setting the profile's file does not have comes from
`config.toml`, which is also what a game played with no profile uses.

The **Platforms** column says which platforms' `config.toml` has the setting.

## Display

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `display.screen_width` | `0` | Android, Switch | Columns of the 480-line picture: 0 for the display's shape, 640 for the Xbox's 4:3. |
| `display.vk_driver` | `""` | Android | The Vulkan driver the Vulkan renderer runs on: empty for the phone's own; or the name of a driver archive (an adrenotools zip) in freedreno_drivers in the game's folder, which Video Setup lists: the Turnip build the app downloads on an Adreno GPU, and any other put or imported there. One that does not load is logged and the phone's own is used. |
| `display.renderer` | `"vulkan"` | Android | How the game draws: "vulkan" (over display.vk_driver; OpenGL ES if Vulkan cannot start, which sets this to "gl") or "gl" (OpenGL ES over the phone's driver). Takes effect the next time the game starts. |
| `display.vsync` | `true` | All | Wait for the display between frames; false draws as fast as possible. |
| `display.max_fps` | `0` | All | With vsync off, the most frames a second: 0 for twice the display's refresh rate, -1 for no limit. |
| `display.show_fps` | `false` | All | Draw the frame rate at the bottom right of a game, counted over half a second: green from 30 frames a second, red below. |
| `display.anti_aliasing` | `"off"` | All | Smoothing of jagged edges, which the Xbox did not have: "off"; "fxaa" or "smaa" smooth the 3D view once it is drawn (the HUD and menus stay sharp); "ssaa2x" draws at twice the resolution each way (four times the work); "msaa2x", "msaa4x" or "msaa8x" draw with that many samples a pixel. Android has "fxaa" for "smaa", and no "ssaa2x". |
| `display.interpolation` | `true` | All | Draw a frame for every display refresh, blending between the game's 30 ticks a second; false keeps the original 30 frames a second. |
| `display.direct_camera` | `true` | Android, Switch | In first person, point the view where the player aims now instead of where the last tick left it: the view turns the frame the stick moves, not up to two ticks (66 ms) later. |
| `display.fov` | `0.0` | All | The first-person view's field of view on foot, in degrees across at 16:9 (20 to 150); 0 keeps the stock view. Vehicles, cinematics and scripted cameras keep their own. |
| `display.viewmodel_fov` | `0.0` | All | The first-person weapon's and hands' field of view, in degrees across at 16:9 (20 to 150); 0 keeps the weapon's stock view, also when display.fov widens the world. |
| `display.viewmodel_visible` | `true` | All | Draw the first-person weapon, hands and what is attached to them. Off, they are not drawn; firing, animation, sound and lights go on. |
| `display.passenger_third_person` | `false` | All | Third-person Warthog and Scorpion passengers. Weapon zoom is disabled. |
| `display.high_res_hud` | `true` | All | Draw the HUD (meters, counters, panels, motion sensor, reticles, waypoints, scopes) from the high-res assets (8x the maps' bitmaps); false draws the maps' own bitmaps. |
| `display.hud_button_style` | `"classic"` | All | High-res gameplay controller buttons: classic uses plain Xbox faces; gems uses shaded Xbox faces; menu_gems uses the menu's button art. Requires display.high_res_hud. |
| `display.high_res_text` | `true` | All | Draw the menus' and HUD's text with the fonts in port/assets/fonts (Overpass) at the resolution the game draws at, and the menus' titles from port/assets/titles; false draws the maps' bitmap fonts and titles. |
| `display.shadow_resolution` | `128` | All | The size the objects' shadows are drawn at, in pixels each way: 128 as on the Xbox, or 256, 512 or 1024 for smoother edges, as soft. |
| `display.big_sprite_limit` | Android: `11`, Switch: `8`, PS5: `11` | All | The most sprites (particles, mist, smoke) drawn in a frame that each cover more than half the view; the others are left out. 11 as on the Xbox; fewer for less overdraw where effects pile up close (the Switch's 8: the falls of the Halo level). Changes at once. |
| `display.shadow_limit` | `16` | All | The most objects' shadows drawn at once: the biggest on the screen keep theirs, and the rest fade out as the crowd moves. 0 for no limit, as on the Xbox (each shadow redraws the level under it: a crowd of them is the most of a busy frame). Changes at once. |
| `display.dynamic_light_limit` | `8` | All | The most dynamic lights (muzzle flashes, plasma, explosions) that light the level at once: the biggest on the screen keep their light on the walls and floor, the rest light only the objects near them. 0 for no limit, as on the Xbox (each redraws the level it reaches twice: a firefight of many is the most of a busy frame's GPU time). Changes at once. |
| `display.specular_light_limit` | `2` | All | Of those, the most that add their specular highlights (the shine on metal and wet floors): the biggest on the screen, the first-person weapon's first. The others still light the level, without the shine. 0 for no limit, as on the Xbox (a near light's highlights cover most of the screen: the costliest pass of a firefight). Changes at once. |
| `display.menus` | `"pc"` | All | The menus: "pc" for the PC version's main menu, "xbox" for the Xbox's. |
| `display.menu_language` | `"maps"` | All | The PC menus' language: "maps" for the language of the Custom Edition whose loc.map is in custom_maps (as that edition shows them; English without one), "english" for English. The port's own lines are English. |
| `display.player_names` | `"all"` | All | In multiplayer, whose names are drawn above their heads: "all", "allies", "enemies" or "none". An enemy's shows only within the motion sensor's reach, in sight and not camouflaged; none show if the gametype's motion tracker shows no players, only allies' if it shows only friends. |
| `display.player_name_scale` | `1.0` | All | How large the players' names are drawn: 1.0 three quarters of the size of the HUD's text, 0.25 to 4. |
| `display.match_timer` | `false` | All | Draw the match clock at the bottom right in multiplayer. It counts up from the game's first tick, which the items' respawn periods count from too: a weapon is back on the map at every multiple of its period on the clock. |
| `display.scoreboard_team_layout` | `"teams"` | All | How the scoreboard lists a team game's players: "teams" in a column for each team (red on the left, blue on the right), "score" all in order of score. |
| `display.scoreboard_background` | `true` | All | Draw a panel behind the multiplayer scoreboard, for clearer text. |
| `display.scoreboard_background_color` | `"16, 16, 16, 150"` | All | The scoreboard panel's colour: "red, green, blue, alpha", each 0 to 255 (alpha 0 is see-through, 255 solid). |
| `display.anisotropic_filtering` | `0` | All | Sharpen textures seen at a grazing angle (floors, walls, terrain), which the Xbox did not: 2, 4, 8 or 16 times; 0 filters as the Xbox did. 4 keeps most of 16's sharpness at less cost on slower GPUs. |
| `display.dxt1_astc` | `true` | Android, Switch | On a GPU without DXT textures (Mali), give it the DXT1 textures as ASTC, which holds them nearly as they are in a quarter of the memory of the 32-bit texels they are otherwise decoded to; false decodes them. |
| `display.latency_meter` | `false` | All | In a network game, your round trip to the host in milliseconds at the screen's top right, with bars (the host: its slowest player's), and "Connection problem" when the host has sent nothing for two seconds; the scoreboard's pings in the same colours, as this machine times them. |
| `display.stats_log` | `false` | All | A line a second in the log with the frame rate, the mode, the map, the renderer and the size of the 3D view, so a graphics bug's report or two builds compared say which of them was running and at what frame rate. Nothing is sent anywhere; off by default. |
| `display.per_pixel_lighting` | `false` | All | Light the models (characters, weapons, vehicles, scenery) for each pixel by the lights the game gives them, without the facets the light of each vertex shows across curved surfaces; false lights each vertex, as the Xbox does. |
| `display.structure_culling` | `31` | All | Which tests skip the level's triangles the camera cannot see, as bits added together: 1 outside the view, 2 groups facing away, 4 triangles facing away, 8 smaller groups, 16 nearer rooms first; 0 walks them as the Xbox does. The console's structure_culling changes it until the game quits. |
| `display.model_detail` | `"stock"` | All | Which of their levels of detail models are drawn at: "stock" chooses by their size on the Xbox's 480 lines, at any resolution; "native" by their size on the display's, so that more detail is kept at a distance at high resolutions; "highest" draws the models "native" does, each at its most detailed level. |
| `display.hdr` | `false` | All | Draw the 3D view into a floating-point back buffer, which keeps what the game's additive effects (explosions, plasma, lights, the sun's glare) add past white, and tone map it before the HUD is drawn: display.hdr_tone_mapping brings it back to the display's range instead of clipping it. false draws into 8 bits a channel, as the Xbox does. |
| `display.hdr_exposure` | `1.0` | All | With display.hdr, the 3D view's brightness in linear light before it is tone mapped, 0.25 to 4.0. |
| `display.hdr_tone_mapping` | `"gt7"` | All | With display.hdr, the curve that fits the brightest light into what the display shows: "gt7" (Gran Turismo 7's: the brightest light turns to white, as film shows it), "neutral" (the picture as drawn, its brightest parts rolled off at their own colour), "aces" (filmic, more contrast) or "reinhard" (softer, and darker throughout). |
| `display.text_chat` | `true` | All | Show the text chat of network games: false hides its lines and keeps a controller's D-pad Up from opening it, as for purists and streamers. |
| `display.chat_style` | `"colors"` | All | The text chat's colours: "colors" (names in their team's colour, the text a pale blue) or "white" (all white, as Custom Edition). |
| `display.viewmodel_shield` | `false` | All | The energy shield's flare on the first-person arms too, as on the body (the stock game draws it on the body only). |
| `display.first_person_legs` | `false` | All | Your own legs in first person, seen looking down: your body as others see it, drawn from the waist down, moving as it moves. |
| `display.overshield_effect` | `false` | All | The overshield as this machine draws it: its flare rising up the body as it charges (2 seconds, 3 from a shield that was down) and staying lit, its energy slower, in its own color (the powerup's), on the first-person arms too with viewmodel_shield; a hit on it plays what the projectile plays on an Elite's shield. Only what is drawn and heard changes. In a network game, the host's network.shield_effects decides, unless it is "players" (each player's own) or the game is co-op. |
| `display.shield_glow` | `false` | All | Energy shields light what is around them as they flare, as plasma does, in the shield's own color. In a network game, the host's network.shield_effects decides, unless it is "players" (each player's own) or the game is co-op. |
| `display.shield_glow_intensity` | `"default"` | All | How far the shield glow reaches: "default" or "light_show" (three times as far, and at full strength from a third of the flare). In a network game, the host's network.shield_effects decides, unless it is "players" (each player's own) or the game is co-op. |
| `display.shield_color` | `"default"` | All | Your energy shield's color, as this machine draws it: "default" (the shield's own) or a multiplayer armor color: white, black, red, blue, gray, yellow, green, pink, purple, cyan, cobalt, orange, teal, sage, brown, tan, maroon or salmon. |
| `display.particle_limit` | Android: `4096`, Switch: `2048`, PS5: `4096` | All | The most particles (sparks, smoke, casings, debris) alive at once; past it the oldest goes to make room for each new one. The Xbox's limit was 1024. 0 for the port's own 8192 (a 64-player firefight fills them, and every one costs the processor and the GPU: 2048 on the Switch, 4096 elsewhere). Changes at once. |
| `display.resolution_limit` | Android: `"native"`, Switch: `"720"`, PS5: `"2160"` | All | **Android:** The most lines the game draws at, no more than the screen has: "native" (the screen's own), "2160", "1440", "1080", "720" or "480" (the game's own), in the screen's shape. More lines are sharper and work the GPU harder. Changes at once. **Switch:** The lines the game draws at: "480" (the game's own), "720", "900" or "1080" (or any number between), no more than the console shows (1280x720 in handheld, 1920x1080 docked), which the picture is scaled up to. Changes at once. **PS5:** The PS5's: the most lines the game draws at, "2160" (3840x2160), "1440" (2560x1440) or "1080" (1920x1080), no more than the display shows (the console's video output), which the picture is scaled to. Changes at once. |
| `display.xbox_buttons` | `false` | Switch, PS5 | Draw the controller's buttons as the Xbox's (the green A, the red B...) in the menus, the game's prompts and the text, instead of this console's own. Changes at the next map (or the next start). |
| `display.dynamic_resolution` | `false` | Switch | While the GPU would hold the game under 55 frames a second (for half a second), draw one step lower (1080 to 900, 900 to 720, 720 to 480: never more), and go back once the frame would fit in 90% of a 60 Hz frame for a second. Video Setup, or here. debug.show_resolution shows it. Changes at once. |

## Audio

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `audio.enabled` | `true` | All | Play sound. |
| `audio.volume` | `1.0` | All | The volume of everything, 0.0 to 1.0. |
| `audio.music_volume` | `1.0` | All | The music's volume, 0.0 to 1.0 (of audio.volume). |
| `audio.effects_volume` | `1.0` | All | The volume of every other sound (effects and speech), 0.0 to 1.0 (of audio.volume). |
| `audio.reverb` | `true` | All | Reverberate the world's sounds as the place the player is in does (the maps' sound environments, as the Xbox's I3DL2 reverb did); false keeps them dry. |
| `audio.resampler` | `"sinc"` | All | How sounds are converted to the output's rate: "sinc" with a band-limited low pass; "linear" between neighbouring samples, brighter for the aliasing it lets through. |
| `audio.voice_chat` | `"push_to_talk"` | All | Talking in network games' voice chat: "push_to_talk" (while controls.push_to_talk is held; the microphone opens the first time), "open_mic" (whenever the microphone hears speech), or "off". Others' voices play whatever this is (audio.voice_volume 0 silences them). |
| `audio.voice_volume` | `1.0` | All | The volume of the other players' voices (0 to 2). |

## Input and controls

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `input.touch_controls` | `"auto"` | Android, Switch | The on-screen touch controls in a game: "auto" shows them on a touchscreen while no controller is connected, "on" also with a controller, "off" never. A device without a touchscreen never shows them. The menus take taps in any case. On Android, the carnage report's CONTINUE and LEAVE buttons follow the same rule (otherwise it shows its A and B prompt). |
| `input.touch_aim_assist` | `true` | Android, Switch | The touch controls' swipe aiming gets a controller's aim assist: the aim slows over a target and follows a moving one. false: none (the bullets' own autoaim stays). |
| `controls.push_to_talk` | `"V"` | All | Voice chat: talk while it is held (audio.voice_chat "push_to_talk"); in a game a controller's D-pad Down talks too, as its D-pad Up opens the text chat. A controller's button may be bound too ("Gamepad DPad Down", "Gamepad LB"; Settings &gt; Controls Setup takes one pressed), which the game then ignores (not a replay's playback, whose controls take every button). |
| `input.gyro_aim` | `"false"` | All | **Android:** Aim with the gyroscope: the phone's on Android; on the Switch the controller's, each local player from their own (a Pro Controller, the console held in the hands, or one Joy-Con; of two, the right one). "false" is the stick only, "true" adds the turn to the view, "zoom" adds it only while the player is zoomed in, for fine aiming. **Switch:** Aim with the gyroscope: the phone's on Android; on the Switch the controller's, each local player from their own (a Pro Controller, the console held in the hands, or one Joy-Con; of two, the right one). "false" is the stick only, "true" adds the turn to the view, "zoom" adds it only while the player is zoomed in, for fine aiming. **PS5:** Aim with the DualSense's gyroscope, each local player with their own. "false" is the stick only, "true" adds the turn to the view, "zoom" adds it only while the player is zoomed in, for fine aiming. |
| `input.gyro_sensitivity_horizontal` | `1.0` | All | How far the view turns left and right for the gyroscope, when input.gyro_aim is true. 1 is the sensor's own turn. 0.25 to 10. |
| `input.gyro_sensitivity_vertical` | `1.0` | All | How far the view turns up and down for the gyroscope, when input.gyro_aim is true. 1 is the sensor's own turn. 0.25 to 10. |
| `input.gyro_invert_horizontal` | `false` | All | Invert the gyroscope's yaw. false: turning the phone or controller right looks right. true: turning it right looks left. The stick's invert is its own. |
| `input.gyro_invert_vertical` | `false` | All | Invert the gyroscope's pitch. false: tilting the phone or controller up looks up. true: tilting it up looks down. The stick's invert is its own. |
| `controls.nintendo_menu_buttons` | `true` | Switch | The menus' buttons as the Switch's own menus have them: the button marked A (on the right) chooses and the one marked B (at the bottom) goes back. false: as on an Xbox controller, the bottom button chooses and the right one goes back. A game's controls are the same either way. The buttons change at once; the menus' button pictures at the next map (or the next start). |

## Game

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `game.overlay_menu` | `true` | All | Holding L and R and pressing the d-pad's down opens the game's overlay menu, and closes it: cheats, the game's speed, the HUD, the camera and the renderer's switches, in a panel at the left of the screen (on Android also three or four fingers on the screen together). While it is open the game gets no input; it is not paused. false: the buttons do not open it. |
| `game.console_log` | `"none"` | All | What the game's console shows on screen of what it logs: "important" (bans, players dropped for cheating, what refuses a command, and the asserts that stop the game), "all" (every line, the game's own chatter too), or "none" (the asserts that stop the game only). What a command prints shows whatever this is, and debug.txt has every line. |
| `game.language` | `""` | All | The language the game asks the Xbox for: "ja", "de", "fr", "es" or "it"; empty for English. The game data decides what is translated. |
| `game.enhanced_animations` | `true` | All | The player bipeds' grenade throws keep their legs moving (crouched, in the air and in a vehicle's seat too), riders' hands leave the grips to throw and reload, and a player turns with the aim while throwing and in the air (so shots after a turn in a jump go to the crosshair); false: the original animations, which freeze the legs and stand a rider up (the turning, which moves where shots go, stays on in a network game for every player). |
| `game.ragdoll` | `"none"` | All | The older ragdoll setting. When game.ragdoll_players or game.ragdoll_ai is not in this file, that one takes this ("woody", "halo2" or "none"). A new file has "none", so both stay off. Once each has its own line, this is not read. |
| `game.ragdoll_players` | `"none"` | All | How a player's body falls, in a campaign or a multiplayer game: "none" (the original death animations), "woody" (a ragdoll of light, freely swinging limbs) or "halo2" (a ragdoll as Halo 2's: a heavy torso, limbs that keep out of it, joints that drag, thrown further by blasts). Settings &gt; Gameplay, or here. A cinematic keeps its own. On a client, and in a game of one machine, the ragdoll moves the body; the host of a network game leaves its bodies as the stock game moves them and only draws them as their ragdolls lie. Each machine draws its own. |
| `game.ragdoll_ai` | `"none"` | All | How an AI's body falls, the same choices as game.ragdoll_players. Settings &gt; Gameplay, or here. A cinematic keeps its own. |
| `game.terminal_output` | `"menus"` | All | The game's messages at the bottom left of the screen ("Shaders preloaded.", what the console's commands print): "none", "menus" (in the main menu, not in a campaign or multiplayer game) or "always". While the console is open they are shown. Settings &gt; Gameplay, or here. |
| `game.custom_edition` | `true` | All | Load and run Halo Custom Edition maps (not those that need OpenSauce): put them and Custom Edition's bitmaps.map, sounds.map and loc.map in the custom_maps folder beside the maps folder; the map lists show them as CUSTOM SINGLEPLAYER and CUSTOM MULTIPLAYER. Their tags are checked as the game's own maps' are before they run; false refuses them. |
| `game.unlock_all_levels` | `false` | All | true: a profile made from now on starts with every campaign level finished on every difficulty, so any level can be played. false: a new profile starts at The Pillar of Autumn and opens each level as the one before it is finished, as on the Xbox. Profiles already made keep their progress; the console's profile_reset_campaign starts one over. |
| `game.downloaded_maps` | `""` | All | Maps played as downloaded ones, besides those map sharing downloaded: their names (bloodgulch, hugeass), separated by commas, or "\*" for every map. A downloaded map's scripts may not change the player's settings or other players' games (debug.txt names what they were refused). |
| `game.maps.path` | `""` | All | The folder the game's maps are read from; empty for the maps folder in the data folder. A relative path is under the data folder. Read once, at the start. The disc's maps are only ever unpacked into the data folder's maps, so a folder named here must hold them already. |
| `game.custom_maps.path` | `""` | All | The folder Custom Edition maps are read from (and map sharing's downloads go to); empty for the custom_maps folder in the data folder. A relative path is under the data folder. Read once, at the start. |

## Bots

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `bots.count` | `0` | All | Computer players (bots) in a Slayer, Capture the Flag or King of the Hill game hosted here, local (split screen), system link or a private internet one (not a public one, which the server browser lists): 0 to 63 join its lobby with the players, as far as the game has room. In a game, the console's add_bot and drop_bot (add_bot_red, drop_bot_blue...) add or take one. 0: none, as in the stock game. |
| `bots.skill` | `"normal"` | All | How well the bots play: "recruit", "easy", "normal", "heroic", "veteran" or "legendary" (how soon they react, how well they aim, how far they see, and how they fight: blows, grenades, strafes, retreats). |
| `bots.teams` | `"even"` | All | In team games: "even" puts the bots on both teams, evening them with the players; "against" puts them all on the team the players are not on. |
| `bots.vehicles` | `1` | All | 1: the bots drive and ride the map's vehicles (Warthogs, Ghosts and Scorpions) to goals a long way off, and man its turrets; 0: they stay on foot. |
| `bots.routes` | `"direct"` | All | The ways the bots go to their goals: "direct" (everyone the shortest way, down the middle into one fight), "lanes" (the attackers spread over the map's side lanes, flanking) or "mixed" (most direct, a few on the side lanes). |

## Network

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `network.shield_effects` | `"stock"` | All | Host: the shield effects every player of a network game sees, whatever their own: "stock", "glow", "light_show", "overshield", "glow_overshield" or "light_show_overshield" (display.shield_glow, its light_show intensity, display.overshield_effect); "players": not synced, each player sees their own settings'. An online co-op game is always each player's own. SHIELD EFFECTS in Server Setup &gt; Player Options writes its choice here. |
| `network.fire_origins` | `true` | All | Host: whether a reported hit the shooter's camera had no line of sight to may still count from where their machine says the shot left, when that is not the camera (a VR build's hand: past a corner the camera is behind), no further than a player leaning out and an arm, and in front of the eye. false: from the camera and vehicles' guns only. |
| `network.address` | `""` | All | This machine's IPv4 address for system link, for a machine on several networks; empty chooses one. |
| `network.broadcast` | `""` | All | Comma-separated IPv4 addresses system link sends its announcements to instead of the local network's broadcast address (for VPNs); empty for the local network. |
| `network.online` | `true` | All | Internet play: hosting makes an invite link (logged, and put on the clipboard) that lets whoever has it join over the internet; opening a link (or copying one before switching to the game) joins. Only people with the invite can join. Off keeps system link to the local network. |
| `network.join_from_clipboard` | `true` | All | Join the game of an invite link found on the clipboard when the game comes to the front. |
| `network.tunnel_port` | `0` | All | The UDP port internet play uses; 0 picks one. A fixed one can be forwarded on the router, for networks whose NAT stops connections. |
| `network.client_timeout_seconds` | `15` | All | Seconds a host waits for a machine it hears nothing from before it drops it (5 to 300; the stock game's 15). Longer keeps players whose connection stalls at the cost of frozen players for longer when one really goes. |
| `network.record_replays` | `false` | All | Each network game is recorded to the data folder's replays/, which the main menu's Theater plays: a host's, all of it (what a client that sees everything is sent); a client's, what its host sends it (far players less often) and its own input. Read at a game's start; the overlay's System folder has it too. |
| `network.replay_hints` | `true` | All | Watching a replay with a controller: the bars of the buttons and of the camera's controls over the seek bar. The replay's pause menu turns them on and off (the north button: Y, the Switch's X, the PS5's triangle). |
| `network.full_detail_to_clients` | `false` | All | Hosting: send every client every player every tick (not those far from its own, or out of their sight, less often) and every damage event (not only those near its players), so that a client's replay holds all of the game. More to send: a client's download grows with the players. Read each tick. |
| `network.replays_kept` | `30` | All | How many replays the replays/ folder keeps: the oldest are deleted as a recording starts; 0 keeps them all. |
| `network.allow_upnp` | `true` | All | Let internet play ask the router (UPnP) to forward its port, for networks whose NAT stops connections: when a player joins this machine's game, and when joining a game takes too long. False never asks. |
| `network.public_lobby` | `true` | All | The server browser: public games are listed through the signalling brokers, and Join Game &gt; Server Browser shows them. False lists no game of this machine's and shows none. |
| `network.host_public` | `true` | All | Whether a new game of Create Game &gt; Internet starts as PUBLIC (listed in everyone's server browser: anyone can see and join it) or, false, PRIVATE (only players with its invite link can join). Server Setup's LISTING changes it for each game. |
| `network.voice_lobby` | `true` | All | Hosting: voice chat in the lobby, before and after a game, where every player hears every other. |
| `network.voice_mode` | `"team_global_enemy_proximity"` | All | Hosting: voice chat in a game: "off"; "team_proximity" (teammates near); "team_enemy_proximity" (anyone near); "team_global" (all teammates); or "team_global_enemy_proximity" (all teammates, and enemies near). A game without teams has only enemies; co-op only teammates. |
| `network.voice_kbps` | `24` | All | Hosting: the voices' quality, in the lobby and in a game, in kilobits a second (8 to 64). |
| `network.voice_proximity` | `15.0` | All | Hosting: how near (world units: 1 is about 3 metres) a player must be to be heard by proximity voice chat (5 to 100). |
| `network.text_chat` | `"on"` | All | Hosting: the text chat: "on" (links in lines are refused), "links" (links allowed) or "off", in either case. Always off in a game a player hosts that is listed in the server browser (a public internet game). |
| `network.votekick` | `true` | All | Hosting: let the players vote to kick a player (the console's votekick; the D-pad's left votes yes, its right no). The yes votes needed are network.votekick_threshold's, counted once per address, and more yes than no. |
| `network.votekick_threshold` | `"dynamic"` | All | Hosting: the yes votes a vote (to kick, to end the game) needs, of the players who may vote: "dynamic", scaled with the players (more than half up to 8, 40% up to 32, 30% beyond); "static", network.votekick_percent of them. At least 2 either way. |
| `network.votekick_percent` | `50` | All | Hosting: with network.votekick_threshold "static", the percentage of the players who may vote whose yes passes a vote (1 to 100, rounded up; a kick's never more than those who can vote). |
| `network.vote_end_game` | `true` | All | Hosting: let the players vote to end the game ("!endgame" in the chat; the D-pad's left votes yes, its right no), counted as a vote to kick. The game ends with its scores, as sv_end_game ends it. |
| `network.votekick_minutes` | `5` | All | Hosting: the minutes a player must have played on this server to start a vote to kick (0 to 60); to vote, 2 minutes or this, the less. |
| `network.votekick_ban_minutes` | `30` | All | Hosting: the minutes a player kicked by a vote cannot join again (1 to 1440). |
| `network.map_share` | `true` | All | Hosting: send a player who joins without the game's Custom Edition map the map (from custom_maps), and the resource maps it needs that the player lacks (bitmaps.map, sounds.map, loc.map). The game's other players help (network.map_seed). |
| `network.map_share_upload` | `1024` | All | Hosting: the most kilobytes a second the host sends of maps (16 to 20480: 20 MB/s), so that the game's own traffic keeps the rest of the upload. |
| `network.map_seed` | `true` | All | Playing in another's game: help send its map to players who join without it, as a torrent's peers do (the pieces this machine has, at most network.map_seed_upload). |
| `network.map_seed_upload` | `256` | All | Playing in another's game: the most kilobytes a second this machine sends of maps to other players (16 to 10240: 10 MB/s). |
| `network.map_download` | `true` | All | Joining a game on a Custom Edition map this machine does not have: download it (and the resource maps it needs) from the game's players into custom_maps, if its host shares it (network.map_share); false leaves the game, saying which map is missing. |
| `network.coop_public` | `false` | All | Whether an online co-op game (Create Game &gt; Internet, a SINGLEPLAYER map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in co-op, which writes its choice here. |
| `network.coop_team_wipe` | `"auto"` | All | When every co-op player dies: "auto" reverts the checkpoint in a PRIVATE game and only respawns in a PUBLIC game; "revert" or "respawn" overrides that choice. A password does not make a PUBLIC game private. Reverting requires a saved checkpoint with the current players and machines; otherwise they respawn without a world reset. TEAM WIPE in Server Setup &gt; Co-op Options writes its choice here. |
| `network.coop_friendly_fire` | `"on"` | All | Whether the players of an online co-op game hurt each other: "off", "on", "shields_only" or "explosives_only" (FRIENDLY FIRE in co-op's Server Setup &gt; Co-op Options writes its choice here). Their AI allies they hurt as network.coop_ally_friendly_fire says. |
| `network.coop_ally_friendly_fire` | `true` | All | Whether the players of an online co-op game hurt their AI allies (the Marines, Captain Keyes, the Sentinels while they are allies), as in the campaign; false, the players' weapons, grenades and vehicles do them no harm. ALLY FRIENDLY FIRE in co-op's Server Setup &gt; Co-op Options writes its choice here. |
| `network.coop_own_friendly_fire` | `true` | All | Whether this machine's players hurt, and are hurt by, the other players of an online co-op game: two players hurt each other only if both have it true. FRIENDLY FIRE in that game's pause menu writes its choice here. A player away from the controls for over 70 seconds is hurt by every player, whatever either chose. |
| `network.coop_player_collisions` | `true` | All | Whether the players of an online co-op game bump into each other; false, they walk through each other (the AI's characters they still bump into). PLAYER COLLISIONS in co-op's Server Setup &gt; Co-op Options writes its choice here. |
| `network.coop_enemies_mode` | `"per_player"` | All | Online co-op's extra enemies: "none", "per_player" (each squad of enemies grows by coop_enemies for each player past the first) or "multiplier" (each is coop_enemies_multiplier times as large, for any number of players). EXTRA ENEMIES in co-op's Server Setup &gt; Co-op Options writes its choice here. |
| `network.coop_pickups` | `"none"` | All | Extra campaign weapons, grenades, health kits, overshields and camouflage. Modes: "none", "per_player" (coop_enemies percent per extra player), or "multiplier" (coop_enemies_multiplier times as many), up to 8 times the original pickups. Only safe nearby places receive copies. |
| `network.coop_vehicles` | `"none"` | All | Extra campaign vehicles: "none", "per_player" or "multiplier". After a player clears its unloaded starting place by both bounding radii plus 15 percent, make one more there, up to floor((players - 1) \* coop_enemies \* 2 / 300) extras, or floor((coop_enemies_multiplier - 1) \* 2 / 3) in static mode. The host alone creates them, and waits while the starting place is blocked. |
| `network.coop_enemies` | `50` | All | Online co-op's extra enemies per player, a percentage: for each player past the first, each squad of enemies a level places gets this much of itself more (100: as many again; 25 to 200). PER PLAYER in co-op's Server Setup &gt; Co-op Options writes its choice here. |
| `network.coop_enemies_multiplier` | `2` | All | Online co-op's shared static multiplier (2 to 32). Pickups are capped at 8 times their original count; extra vehicle growth uses two thirds of (multiplier - 1), rounded down. MULTIPLIER in co-op's Server Setup &gt; Co-op Options writes its choice here. |
| `network.brokers_file` | `"brokers.txt"` | All | The file of the public MQTT brokers through which the machines of an invite find each other (its messages are encrypted), beside this file unless a full path: one host:port on each line, up to 4. Updates replace brokers.txt: keep a list of your own under another name. |
| `network.listings` | `"both"` | All | What the server browser lists: "both" (dedicated servers from the master servers first, then the games players host, from the brokers), "masters" (the master servers' alone) or "brokers" (the brokers' alone). |
| `network.masters_file` | `"masters.txt"` | All | The file of the master servers the server browser asks for dedicated servers, beside this file unless a full path: an https:// URL and its key (ed25519:&lt;hex&gt;) on each line, up to 4. None named (no file, or comments alone): the official master, built in. |
| `network.link_share` | `"when_needed"` | All | When the game shows a server the Discord account linked to it (Settings &gt; Network Setup &gt; Discord Account): "when_needed" (a server that asks, to let you in from a VPN), "always" or "never". |
| `network.browser_source` | `"all"` | All | The server browser's games: "all" (ALL GAMES: the dedicated servers and the games players host), "dedicated" (the master servers' dedicated servers) or "players" (P2P GAMES: games players host). Its top-left tab changes it. |
| `network.browser_show_empty` | `true` | All | The server browser lists games with no player (its Filters screen). |
| `network.browser_show_full` | `true` | All | The server browser lists full games (its Filters screen). |
| `network.browser_maps` | `"yes"` | All | The server browser lists games on maps this machine lacks: "yes", "no", or "downloadable" (those of a Custom Edition map its host shares). |
| `network.browser_gametype` | `"any"` | All | The server browser lists games of one kind only: "any", "ctf", "slayer", "oddball", "king" or "race" (its Filters screen). |
| `network.browser_sort` | `"players"` | All | The server browser's order: "players" (the most players first), "servers" (dedicated servers first, then by players), "name", "map", "type" or "locked" (a password's first); "-" before one reverses it ("-players"). A column title sorts by it, and again the other way. |
| `network.stun_servers` | `"stun.l.google.com:19302,stun.cloudflare.com:3478"` | All | Public STUN servers that tell this machine its internet address; comma-separated host:port. |
| `network.link_page` | `true` | Switch, PS5 | While Join Game &gt; Direct Link is open, serve a page on the local network (its QR code and address are on that page) where a phone or a computer pastes an invite link for the console to join; nothing is served otherwise. false: none. |

## Performance

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `performance.cpu_clock` | `"1785"` | Switch | The CPU's clock: "1785" (1.7 GHz, the most Nintendo's own games use), "1020" (1 GHz, the console's handheld rate: cooler, and the battery lasts longer) or "stock" (left as the console has it). Changes at once. |

## Updates

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `update.auto` | `true` | All | Look for a new version when the game starts, and offer to update to it; false never looks (the game's "Do not ask again" writes false here). |

## Streamer mode

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `streamer.enabled` | `false` | All | Hide offensive words in names and text from other players, maps and mods as stars, at the levels below (Settings &gt; Network Setup &gt; Streamer Mode). |
| `streamer.slurs` | `"mild"` | All | With streamer.enabled, the slurs (race, sexuality, gender, disability) hidden: "off", "severe", "strong" (and severe) or "mild" (all). |
| `streamer.swearing` | `"strong"` | All | With streamer.enabled, the swearing and sexual words hidden: "off", "severe", "strong" (and severe) or "mild" (all). |
| `streamer.insults` | `"off"` | All | With streamer.enabled, the insults that are not slurs hidden: "off", "severe", "strong" (and severe) or "mild" (all). |

## Debugging

For finding problems and measuring the game. You should not need these to play.

| Setting | Default | Platforms | What it does |
| --- | --- | --- | --- |
| `debug.show_resolution` | `false` | All | Show the frames a second and the resolution the game draws at, in the bottom right corner ("(dynamic)" while display.dynamic_resolution has dropped a step). Changes at once. |
| `debug.original_particles` | `false` | All | Move the particles that are not attached to an object (sparks, casings, debris) at every frame drawn, as before, rather than once a 30 Hz tick as on the Xbox and drawn between their ticks (which spares the CPU two in three of their collision tests at 60 frames a second, more at 120). Takes effect at the next map. |
| `debug.benchmark` | `""` | All | A benchmark run, the same each time: "standard" plays a10, a30, b30, c10 and c40 30 seconds each and Blood Gulch, Damnation, Sidewinder and Chill Out 15 seconds each with 63 bots, the view turning, and writes the frame rates to benchmark.txt (in the data folder, or the run's profile folder: debug.profile_run); or a list of map:seconds ("a30:30,bloodgulch:15"). Empty: none, the game as it is. |
| `debug.benchmark_bots` | `63` | All | The computer players in the benchmark's multiplayer games: 63, with the player 64 in all; 0 to 63. |
| `debug.benchmark_manual` | `false` | All | The benchmark leaves the player's view to the player rather than turning it, to measure the game as it is played (the player still cannot be hurt). |
| `debug.benchmark_quit` | `true` | All | Quit the game once the benchmark is done; false goes back to the main menu. |
| `debug.core_dump_tick` | `0` | All | A core of the first level played (d:\core\game_state_&lt;address&gt;.bin, named for where the game state is) written at this game tick, for comparing saved games across runs; 0 for none. |
| `debug.core_load` | `""` | All | A core (a file in d:\core, as debug.core_dump_tick writes) loaded into the first level played at debug.core_dump_tick, in place of writing one: a core of a run whose window lay elsewhere, for trying saved games across windows; empty for none. |
| `debug.network_test` | `""` | All | Automated system link sessions for testing: "host:&lt;map&gt;" hosts a game on that map, "local:&lt;map&gt;" plays a local (split screen) game on it, "join" joins the first game found, "browse" the one with the most players the server browser lists (not co-op, if it can), "log" logs the game that runs (and runs its cues); empty for none. |
| `debug.network_test_start` | `15.0` | All | Seconds after hosting that an automated test game starts. |
| `debug.network_test_kill` | `0.0` | All | Every this many seconds an automated test host kills its last player; 0 never. |
| `debug.network_test_score` | `0` | All | The score an automated test host's game type plays to (a short game, to test the next); 0 the game type's own. |
| `debug.network_test_shoot` | `0.0` | All | Every this many seconds each automated test player hits the next with their weapon, within its reach (the host brings far players near the first a second before); 0 never. |
| `debug.network_test_vehicle` | `0.0` | All | This many seconds into an automated test game the host seats its last player as a vehicle's driver (and out 15 seconds on); 0 never. |
| `debug.network_test_pickup` | `0.0` | All | This many seconds into an automated test game the host stands its last player on a weapon, which a joining player then picks up; 0 never. |
| `debug.network_test_chat` | `0.0` | All | This many seconds into an automated test game each machine's player says a chat line to everyone, then to the team and to the vehicle (the host seats the last two players in one first), then the host mutes and unmutes a player, refuses a link and turns the chat off, and the clients type on the on-screen keyboard; 0 never. |
| `debug.network_test_name` | `""` | All | The name this machine's player takes in an automated test game (ASCII); empty keeps its profile's. |
| `debug.network_test_pickup_weapon` | `""` | All | The weapon network_test_pickup stands the player on: the first whose tag name has this in it ("sniper", say); empty any. |
| `debug.network_test_cues` | `""` | All | Console lines an automated test runs at a game second, from the first tick every player of debug.network_test_cue_players has a unit: "&lt;second&gt; &lt;line&gt;\|&lt;second&gt; &lt;line&gt;..." (the host's, or a game of one machine). |
| `debug.network_test_cue_players` | `1` | All | The players debug.network_test_cues waits for. |
| `debug.network_test_public` | `false` | All | The network test's host lists its game in the server browser, as Create Game &gt; Internet's PUBLIC does (debug.network_test "browse" joins it). |
| `debug.telnet_console` | `false` | All | Listen on 127.0.0.1 (port telnet_console_port) for a script console that runs what it is sent as the game's console does, with no password; false none. |
| `debug.telnet_console_port` | `2323` | All | The port of the script console (telnet_console); the Xbox's was 23, which only the administrator can listen on. |
| `debug.touch_targets` | `false` | All | Outline the menus' tap targets (item green, value blue, list slot yellow, legend button red, the band beside a list's slots orange; the virtual keyboard's keys white), mark where the last finger went down and the last tap landed for 3 seconds, and log each tap with the target it hit (and a value's split); to judge touch accuracy. |
| `debug.log_addresses` | `false` | All | Write other machines' internet addresses whole in debug.txt and the console, for debugging your own network. False writes each public address as a tag (addr#3f2a9c) that only matches up within one run; local network addresses are always whole. Do not post a debug.txt written with this on: it has the address of everyone you played with. |
| `debug.network_latency` | `0.0` | All | Milliseconds everything received is held back (a round trip between two machines of twice it), to test the netcode as over the internet; 0 none. |
| `debug.network_loss` | `0.0` | All | Percent of datagrams received that are dropped, for the same; 0 none. |
| `debug.network_corrupt` | `0.0` | All | Percent of the datagrams received that are damaged at random, to test that nothing a machine sends can crash the game; 0 none. |
| `debug.network_corrupt_stream` | `0.0` | All | Percent of the reads of streams that are damaged at random, for the same (a damaged stream is closed, so a little goes a long way); 0 none. |
| `debug.network_corrupt_after` | `0.0` | All | Seconds after the start before anything is damaged, so that a game can be set up and started first (a host's messages to its own client are damaged too). |
| `debug.test_input` | `""` | All | "bot:&lt;seed&gt;" plays controller 1 with a scripted pattern (automated network tests); "look:&lt;seed&gt;" stands still, only turning and looking up and down; "menu:&lt;buttons&gt;" presses menu buttons one a second (a, b, x, y, up, down, left, right, start, back or wait, separated by spaces or commas), for testing the menus; empty for none. |
| `debug.bot_trace` | `0` | All | Log each bot's state (bots.count) every this many seconds: where it is and goes, its path, target and weapon; 0 never. |
| `debug.fixed_step` | `false` | All | A game of this machine alone runs one tick a frame, whatever time has passed, so that it plays the same every time (tests); game_speed has no effect. |
| `debug.exit_after` | `0.0` | All | Quit this many seconds after the game starts; 0 never. |
| `debug.voice_test` | `false` | All | Voice chat's automated tests: a tone instead of the microphone, and each voice heard logged once a second. |
| `debug.null_renderer` | `false` | All | Run drawing nothing. |
| `debug.gl_debug` | `false` | All | Report OpenGL errors in the log. |
| `debug.menu_open` | `""` | All | Start on this screen of the menus instead of the main menu, a player profile being edited; empty for the main menu. |
| `debug.gpu_stats` | `false` | All | Log the renderer's draw counts once a second (otherwise, on Android's Vulkan, only a second whose draws were skipped). |
| `debug.gpu_trace_frame` | `-1` | All | Log every draw of this frame; -1 none. |
| `debug.gpu_trace_constants` | `false` | All | With gpu_trace_frame, also the vertex shader constants. |
| `debug.gpu_skip_vertex_shaders` | `""` | All | Comma-separated ids of vertex shaders not to draw with. |
| `debug.gpu_dump_shaders` | `""` | All | A folder to write the generated GLSL to; empty none. |
| `debug.gpu_debug_expression` | `""` | All | A GLSL expression every pixel shader shows instead of its result. |
| `debug.gpu_debug_texture0` | `false` | All | Pixel shaders show their first texture. |
| `debug.gpu_debug_flat` | `false` | All | Pixel shaders show their vertex colour. |
| `debug.screenshot_directory` | `""` | All | A folder to save frames to (with screenshot_every); empty none. |
| `debug.screenshot_every` | `0` | All | Save every this many frames to screenshot_directory; 0 none. |
| `debug.texture_dump_directory` | `""` | All | A folder to write every texture to as it is uploaded; empty none. |
| `debug.log_files` | `true` | All | Write the game's log to debug.txt, beside the data. false writes none, which spares the storage a write for each line; nothing is kept to say what went wrong. Changes at once. |
| `debug.halo_log` | `true` | All | Write the program's log to halo.log too, begun again each start: beside the data on Android (logcat's lines of tag "halo"), beside the program on the Switch (nxlink's), in /data/halo on the PS5 (the kernel log's). false writes none. Takes effect the next start. |
| `debug.texture_log` | `false` | All | Log texture uploads. |
| `debug.texture_no_cache` | `false` | All | Upload textures again every time they are used. |
| `debug.sample_seconds` | `0.0` | All | Log where every game thread is this often, in seconds; 0 never. |
| `debug.memory_watch` | `true` | All | Notice the game's writes to cached textures and vertices by page protection; false compares page contents once a frame instead, which is slower. Under ARM translation (the x86 emulator) the app always compares contents. Read by the host from the file. |
| `debug.original_sun_glow` | `false` | Android | Make the sun's glow from a copy of the screen in the middle of each frame, as the game does, rather than at the frame's end for the next frame (which spares the GPU storing the screen and loading it back; the glow follows what is in front of the sun a frame later). Takes effect the next start. |
| `debug.original_water` | `false` | Android | Make the water's ripples in the middle of each frame, as the game does, rather than at the frame's end for the next frame (which spares the GPU storing the screen and loading it back; the ripples move a frame later). Takes effect the next start. |
| `debug.original_multisample` | `false` | Android | Under Vulkan with display.anti_aliasing's multisampling: draw the colour into images of the samples beside the screen's, resolved as each rendering ends, rather than keeping its samples in the GPU's tile memory only (where the phone's driver can, VK_EXT_multisampled_render_to_single_sampled), which spares storing and loading them at every break of the frame. Takes effect the next start. |
| `debug.gpu_split_parts` | `false` | Android | With debug.profile_run under Vulkan: end the GPU's rendering at each part of the frame the game names (the sky, the objects, the particles...), so that gpu.txt times each part on its own. The stores and loads it adds make every frame slower: for ranking the parts, in a run of its own. Takes effect the next start. |
| `debug.profile_run` | `false` | Android | Profile each run, to send to the developers: a folder of its own in the phone's Download/halo_profiles (named for the day and time it started: DDMMYYYY_HHMMSS) gets where the game's threads spend their time (profile.bin, sampled debug.profile_hz times a second, 500 if that is 0), the GPU's time by part of the frame (gpu.txt, under Vulkan), the log and the phone's details (run.txt). Android 9 and 10 ask for storage once. Costs a little speed. Takes effect the next start. |
| `debug.gpu_draw_scissor` | `true` | All | **Android:** Rasterize an object's shadow and a dynamic light's pass only inside the rectangle of the screen they can change; false rasterizes their whole triangles, to compare. Takes effect the next start. **Switch:** Rasterize an object's shadow and a dynamic light's pass only inside the rectangle of the screen they can change; false rasterizes their whole triangles, as before, to compare. Takes effect the next start. **PS5:** Rasterize an object's shadow and a dynamic light's pass only inside the rectangle of the screen they can change; false rasterizes their whole triangles, to compare. Takes effect the next start. |
| `debug.gpu_texture_clip` | `"on"` | All | Shadows cast on the level drawn only over their map: "on", "off", or "wide" (clipped by limits nothing reaches, to tell the clip's own effect). Takes effect the next start. |
| `debug.original_zoom` | `false` | All | **Android:** Draw the zoom's blur as the game does, in passes over the whole screen, rather than on a small copy blended over it once. Takes effect the next start. **Switch:** Draw the zoom's blur as the game does, in passes over the whole screen, rather than on a small copy blended over it once (about 3.7 ms less of the GPU's frame at 720p). Takes effect the next start. **PS5:** Draw the zoom's blur as the game does, in passes over the whole screen, rather than on a small copy blended over it once. Takes effect the next start. |
| `debug.original_blur` | `false` | All | **Android:** Draw a blur screen effect (the cryo bay's waking up, cutscenes) as the game does, in passes over the whole screen, rather than its passes on smaller copies of it. Takes effect the next start. **Switch:** Draw a blur screen effect (cutscenes) as the game does, in passes over the whole screen, rather than its passes on a small copy of it. Takes effect the next start. **PS5:** Draw a blur screen effect (the cryo bay's waking up, cutscenes) as the game does, in passes over the whole screen, rather than its passes on smaller copies of it. Takes effect the next start. |
| `debug.vk_present_marker` | `false` | Android, PS5 | Under the Vulkan renderer, draw a red square at the top left and a green one at the top right of the picture, so that a screenshot shows which way up it is. |
| `debug.vk_static_geometry` | `true` | Android, PS5 | Under the Vulkan renderer, draw the map's geometry (written once as it loads) from a copy made the first time it is drawn, rather than copying it at each draw. false copies it at each draw, as before. |
| `debug.custom_edition_elsewhere` | `false` | Android, PS5 | Put Custom Edition's tag cache somewhere other than 0x40440000 even where that address is free, and move its maps' addresses there, as on a phone whose Java runtime holds 0x40440000. For testing that move. |
| `debug.vk_static_check` | `false` | Android, PS5 | Under the Vulkan renderer, compare the game's static geometry with what was copied every 60 frames and log a range that changed (it would be drawn stale). |
| `debug.vk_render_thread` | `true` | Android | Under the Vulkan renderer, record the game's commands, hand them to the driver and present on a thread of their own, while the game makes the next frame. false runs them on the game's thread, as before. |
| `debug.vk_self_test` | `false` | Android, PS5 | Under the Vulkan renderer, try the renderer's clears and the copying of a draw's data on small targets of its own at start-up; the log says ok or FAILED. |
| `debug.vk_probe` | `""` | Android | Run the Vulkan probe instead of the game: "all", or a list of caps, memory, compile, pipelines, draw, present; empty runs the game. It writes vk_probe.txt. |
| `debug.vk_validation` | `false` | Android, PS5 | Turn on Vulkan's validation layer in the probe and in the Vulkan renderer, when the app carries it. |
| `debug.profile_hz` | Android: `0`, Switch: `500` | Android, Switch | **Android:** Sample every game thread this many times a second into profile.bin; 0 off. **Switch:** The profiler's samples a second, when debug.profiler is true. |
| `debug.resolution_uncapped` | `false` | Switch | Draw at display.resolution_limit's lines even past what the console shows, scaled down to it: a Switch Lite (720p) drawing 1080p, to test a docked console's GPU load. Changes at once. |
| `debug.profiler` | `false` | Switch | Profile the game: sample where its threads are (debug.profile_hz times a second) and write the counts to the profile folder every 20 s, for tools/switch_profile.py. Takes effect the next start. |
| `debug.mic_probe` | `false` | Switch | For two minutes from the start, log what the console's microphone input hears: its inputs, the format it gives and the sound's level each second. Takes effect the next start. |
| `debug.texture_verify_compressed` | `false` | Switch | Read each DXT-compressed texture back out of its image after it is written, and log where it differs from the game's bytes. Slow: it waits for the GPU each time. Takes effect the next start. |
| `debug.gpu_trace` | `false` | Switch | Keep a record of the GPU's last commands, and after a GPU fault write it and the console's account of the fault to halo.log. It costs the GPU some time. Takes effect the next start. |
| `debug.gpu_fault_test` | `false` | Switch | Make the GPU fault on purpose at the 1200th frame, to see gpu_trace's record. Takes effect the next start. |
| `debug.gpu_profile` | `false` | Switch | Every 300 frames, log the GPU's time by part of the frame (the sky, the objects, the fog...) and what a frame asks of it. Takes effect the next start. |
| `debug.gpu_capture` | `false` | Switch | With gpu_profile, time one frame of every 300 draw by draw, and log the GPU's time by shader, target and kind of command. Takes effect the next start. |
| `debug.gpu_experiment` | `""` | Switch | Measurements of what a change could save, comma-separated: "no_compression" (render targets without hardware compression), "no_target_barriers" (no wait before a render target change; may draw wrong), "lod_bias" (textures sampled three levels smaller: blurry). Empty for none. Takes effect the next start. |
| `debug.gpu_shader_ranges` | `true` | Switch | Leave out of the pixel shaders the clamps their values cannot need; false writes them all, as before, to compare the two. Takes effect the next start. |
| `debug.mod_models_in_heap` | `false` | Switch | Keep the mods' models outside the Xbox's contiguous memory, as a model past their budget is, to try that path with any mod. Takes effect the next map. |
| `debug.gpu_blend_discard` | `true` | Switch | Let a blended draw's pixels go where they would leave the screen as it was (the empty parts of particles and glows); false blends them all in, as before, to compare. Takes effect the next start. |
| `debug.render_thread` | `true` | Switch | Turn the game's drawing commands into the GPU's on a thread of their own, on another core, while the game goes on with its next frame; false runs them on the game's thread, as before. Takes effect the next start. |
| `debug.gpu_glsl_folder` | `""` | Switch | A folder to write each shader the game draws with to, as GLSL named by its hash (the names gpu_capture logs); the folder must exist. Empty for none. |
| `debug.profile_record`, `debug.profile_record_when`, `debug.profile_memory` | `false`, `"start"`, `256` | Android | Profiling builds only ([docs/building.md](building.md#profiling-builds)). Record without a command; `"start"` records from the first frame until the first map change, `"game"` records each game outside the main menu. Memory is 4 to 1024 MB. |
