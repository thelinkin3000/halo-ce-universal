# Theater: cinematic camera enhancements

The plan and the decisions for the Theater's cinematic and free cameras, from
*Cinematic Camera System: Feature Addendum (v2.0)*. The addendum is kept
outside the repository (`Cinematic Camera Feature Addendum (4).md`, in the
user's Downloads), so what it asks for is summarised here.

How the cameras behave now is described in `port/linux/REPLAY.md` and at the
top of `port/linux/game/network_replay_playback.c`.

## Status

- Steps 1 to 7 are written, on the branch `theater-cinematic-camera`.
- **Not yet run in the game.** Android, Switch and PS5 build and link. The
  spring's maths and the moving camera's motion were tested on their own, against
  a stubbed collision layer (see step 6). Nothing about how it looks has been
  checked, nor anything against a real level's geometry.

## The addendum

Three goals:

1. No clipping into geometry or the player.
2. A dynamic, varied camera operator.
3. Better framing and action visibility.

| Item | Asks for | Tagged for | Outcome |
|---|---|---|---|
| 1.1 | Wall clearance: a 20-30 cm sphere sweep | Cinematic, Creator | Done, with rays (see below) |
| 1.2 | Anti-stutter: ray bundle, fast in, slow out, 150 ms hold | Cinematic, Creator | Done, applied to cuts (see the moving camera's decisions) |
| 1.3 | Corner fallback: crane or side shift | Cinematic, Creator | Done, at placement |
| 2.1 | Terrain and elevation awareness | Cinematic, Creator | Done, at placement |
| 2.2 | Elastic distance tether | Cinematic | Done, as a chase that ends in a tripod (the user's change) |
| 3.1 | Rule of thirds, lead room | Cinematic, Creator | Done |
| 3.2 | Off-screen threat framing | Cinematic | Done |
| 3.3 | Arc-orbit sweep | Cinematic | Done, only during the chase (the user's change) |
| 3.4 | Spring-damped smoothing | Cinematic, Creator | Done |

"Creator" is the free camera while it tracks the player.

## Decisions

**The tripod is the default; moving is the exception (the user's decisions).**
The cinematic camera is a tripod whenever the player is within its maximum
distance: it turns, frames and zooms, and does not move, whatever they do. Past
that distance it moves in to a third of it, then is a tripod again. The free
camera does not get any of this, as the addendum tags the tether and the arc for
Cinematic only.

**A steady run gives repeated chases.** With the tripod the default, a player who
keeps running gets a chase, a stop, and another chase, rather than a camera that
keeps following. Reaching a third of the distance takes the spring being led by
their speed, as a plain spring would lag behind and never arrive. This is what was
asked for, and how it looks is the first thing to check in the game.

**It moves round the player, not across the ground.** Its position is taken
as an angle and a length about the player each frame, and the arc and the tether
each change one of them on a spring. So the arc is a true arc, and nothing has to
be remembered about where the camera was meant to be: a wall that stops it is
simply where the next frame starts from.

**The tripod's zoom is back, at its original distance.** It is how the tripod keeps
the action in frame without moving, and it is continuous as the distance changes,
so a chase's coming in reads as the player staying the same size. At the maximum
distance that is about 32 degrees on foot and 16 in a vehicle.

**Its motion goes by the game's time.** The aim and zoom springs still go by real
time. If the camera's own moving did too it would drift round a paused player, and
at 4x it would fall four times as far behind.

**The arm's fast-in, hold and slow-out were removed.** The addendum's timing is for
an arm kept at a length. This camera only ever comes in, and once it is in range it
is a tripod, which by the decision above does not move out again, so there is
nothing to recover. They were written for the earlier version and took the camera
out of its tripod state against a wall. What is left: where the place it is going
cannot see the player it comes only as far as it can see them from, and where that
is under half of the way the arc stops, leaving a real wall to the sight check's
cut. So a cut is still the answer to losing the player entirely.

**A jump of the player is a cut.** A teleporter, a spawn or a jump in the replay
moves the focus further and faster than any vehicle (`CINEMATIC_JUMP_SPEED`). The
camera would otherwise fly across the level, so it is placed anew. The pull is
capped too (`CINEMATIC_TETHER_MOST_PULL`), for a camera a corner lets go; the
lead for the player's speed is on top of the cap, so that it can still catch them.

**The arc rests during a threat.** The threat's framing was worked out against
a camera that is not going anywhere, and two moves at once would read as neither.
The chase goes on.

**Order of the work.** Fixing the cuts came first, as they were a likely bug.
Then the spring, as everything else moves through it, then framing and
clearance, then placement, then the threat.

**The behind view is unchanged.** The new placement is its own function,
`playback_free_camera_place`, rather than a change to `playback_behind_camera`.
Checking scenery and doors every frame there would have made that view jump in
front of them, and the addendum does not ask for it.

**Rays rather than a sphere sweep.** `collision_test_sphere` only reports an
overlap and there is no sweep to use, so clearance uses the addendum's other
option, a bundle of `collision_test_vector` rays. The rays spread from the
player's focus to the end of the arm, so they all start somewhere clear.

**The margins stay as they were.** The existing 0.15 and 0.2 world units are
about 45 and 60 cm (a unit is 10 feet), already more than the 20-30 cm asked for.

**The anti-stutter rules apply to cuts.** A tripod has no spring arm to collapse
and recover, so the fast-in and slow-out timing has nothing to act on. What could
stutter is the cutting, so the minimum shot and the 150 ms hold go there.

**A hidden player can stay hidden a little longer.** The minimum shot means a
player can be out of sight for up to about a second just after a cut. That is
the cost of not cutting repeatedly. `CINEMATIC_LEAST_SHOT` controls it.

**Lead room is on the free camera too**, as the addendum tags it for Creator, so
that camera no longer keeps the player centred while it tracks. It is measured
from the camera's line to the player rather than from its aim, so that the
framing cannot feed back into itself.

**Threat framing is judged against the framing it would otherwise have**: the
aim and the width the zoom wants, not the current ones. Otherwise widening for a
threat would put them on the screen, which would end the widening, and so on.

**The threat's hold is counted in the game's time, by tick.** So it follows the
replay's speed, and a jump back in the replay is simply no hit yet.

**The replay format does not change.** Damage events already carry the attacker
(`owner_player_index`, `owner_object_index`) and `origin`, and every event goes
to the recorder, so old replays should get threat framing too. A client's
recording has only the events that client received, so it may miss some hits.

**Thresholds were chosen without seeing them on screen.** Where the addendum
gave a number it was used where it applied (the 150 ms hold, a hold within its
1.0-1.5 s window). The rest are first guesses and will probably need tuning.

## What was built

### Step 1: the cuts

The bug: placement checked only the level's structure, but the sight check also
counted scenery and doors. With scenery behind the player the camera could land
behind it, be hidden at once, and re-cut every 0.2 s. This was found by reading
the code, not seen in the game.

- Placement now checks the same things as the sight check.
- A shot lasts at least `CINEMATIC_LEAST_SHOT`, except just after a spawn.
- The player is looked for at their middle and either side, so that a post does
  not hide them (`playback_free_camera_sees`).
- The time hidden starts over only after `CINEMATIC_CLEAR_TIME` in sight, so a
  run past a row of posts counts as the hiding it is.

### Step 2: the spring

`playback_spring` is an exact critically damped spring, so the result does not
depend on the length of the frames. It replaces `playback_ease` on the aim and
the zoom. Its `time` is how far it lags a target moving at a steady speed, so
the old constants keep their meaning. Tested on its own:

- The same result at 60, 20 and 1 frames a second.
- No overshoot from rest.
- 2% of the way in the first frame at 60 fps, where the old easing went 10.5%.
- A measured lag of `time` less half a frame, at every frame rate from 20 to
  10000 fps. The half frame is because the target moves once a frame.

### Step 3: framing and clearance

- `playback_free_camera_framing`: the player on a third-line with the room ahead
  of where they aim, scaled by how side-on they are to the camera.
- `playback_view_tangents`: how a view's width is the screen's, from main.c's
  vertical tangent and the window's shape, so that the third-line is in the right
  place at any aspect ratio.
- `playback_free_camera_reach`: 5 rays for how much of the arm is clear.
- `playback_free_camera_clear`: a moving free camera backs away from corners it
  has come too close to. It only runs while the camera moves.

### Step 4: placement

`playback_free_camera_place` tries behind the player, a crane, then either side,
and takes the first with at least `FREE_CAMERA_LEAST_ARM` of the arm and sight of
the player. If none has both, it takes the best it found.

`playback_free_camera_terrain_rise` looks at the ground in 8 directions and uses
the 4th highest, so a wall on one side does not raise the camera but a valley
does. It does nothing under a roof.

A placement costs about 40 rays. It is rare, but its cost has not been measured
on the Switch.

### Step 5: the threat

`network_damage_handle_events` tells `network_replay_playback_damage` about a
hit that did harm, and `playback_cinematic_threat` frames it. A hit is ignored
if it is the player's own or nobody's (a fall). The aim goes halfway between the
player and the attacker, the view widens as far as it must up to
`CINEMATIC_THREAT_WIDEST`, and past that the player is kept at the edge of the
frame with the aim as near the attacker as that allows.

It ends after `CINEMATIC_THREAT_HOLD` of game time from the last hit, or sooner
if the attacker would be on the screen anyway, the player aims within
`CINEMATIC_THREAT_FACED` of them, or the player is out of their sight.

### Step 6: the chase

`playback_cinematic_move`, called each frame before the aim so that the aim is
from where the camera now is. Past `CINEMATIC_TETHER_ARMS` of the arm it was placed
with (12.25 units on foot, 24.5 in a vehicle) `cinematic_moving` is set and the
camera comes in to `CINEMATIC_TETHER_SETTLE` of that (4.08 and 8.17), and is a
tripod within `CINEMATIC_TETHER_ARRIVED` of it. Moves go through
`playback_free_camera_move` and `playback_free_camera_clear`, the place's sight
through `playback_free_camera_sees` and step 3's `playback_free_camera_reach`.

Tested by compiling the real functions into a harness with the collision stubbed
as one flat wall, so this says nothing yet about real geometry:

- 12 units away, strafing and spinning for 10 seconds: no movement and no rays.
- Just past the distance, then standing still: a gentle start, 6 units a second at
  most, a tripod after 3.2 seconds at about 4.1 units, at 20, 30, 60 and 144 fps.
- Running at a steady 2.5 units a second: repeated chases, one about every 6.5
  seconds, a tripod 53% of the time, 4.2 to 12.3 units away.
- A vehicle at 10 units a second: one chase about
  every 5 seconds, a tripod 36% of the time, 8.3 to 24.9 units away. At 20 it
  still keeps within 26.
- A camera held at a wall with the player beyond it: it stays off the wall, for the
  sight check to cut.
- A camera 60 units away let go: 16 units a second at most.

Its cost, counted from the code and not measured: no rays of its own as a tripod,
and a tripod's sight check as before. In a chase, about 9 rays a frame in the open
(a sight check, the place's sight, the sweep, and 6 for the clearance), and about
20 at worst against a wall.

### Step 7: the arc

`playback_cinematic_orbit`, during a chase only. Moving across its view or turning
for `CINEMATIC_ORBIT_AFTER` starts it, and it goes on until they have neither moved
nor turned for `CINEMATIC_ORBIT_REST`. It goes to `CINEMATIC_ORBIT_QUARTER` either
side of behind where they aim, on the side it is on, which it keeps until it is
well round the other, so that aiming a little either way does not swap sides. A
chase is short, so it may not get all the way round before the camera stops.

Tested in the same harness:

- With nothing in the way, the radius does not change as it swings.
- A chase with the player strafing got 28 degrees round, and the swing eased off
  for another 6 degrees over 2 seconds after the camera became a tripod.
- By a wall, it stays off it and is a tripod once it is in range (2.2 seconds there).
- It does not swing at all during a threat, or while the replay is paused.

## The numbers

World units and seconds unless marked.

| Constant | Value | What it does |
|---|---|---|
| `FREE_CAMERA_TRACK_TIME` | 0.15 | Aim's lag behind the player (unchanged) |
| `FREE_CAMERA_ZOOM_TIME` | 0.3 | Zoom's lag (unchanged) |
| `FREE_CAMERA_FRAMING_TIME` | 0.4 | Framing's lag |
| `FREE_CAMERA_THIRD` | 1/3 | Lead room, of the way from the middle to the edge |
| `FREE_CAMERA_PROBE_RADIUS` | 0.15 | Spread of the rays at the arm's end |
| `FREE_CAMERA_LEAST_ARM` | 0.5 | Least of the arm a place may keep |
| `FREE_CAMERA_CRANE_ACROSS` | 0.45 | Crane's distance across, of the arm |
| `FREE_CAMERA_CRANE_UP` | 0.95 | Crane's height, of the arm |
| `FREE_CAMERA_TERRAIN_DIRECTIONS` | 8 | Directions the ground is looked at |
| `FREE_CAMERA_TERRAIN_LOOK` | 3.0 | How high it looks from |
| `FREE_CAMERA_TERRAIN_LEAST` | 1.0 | Height above the feet that counts as a valley |
| `FREE_CAMERA_TERRAIN_MOST` | 2.0 | The most the camera rises |
| `CINEMATIC_HIDDEN_TIME` | 0.2 | Time hidden before a cut (unchanged) |
| `CINEMATIC_CLEAR_TIME` | 0.15 | Time in sight for that to start over |
| `CINEMATIC_LEAST_SHOT` | 1.0 | Shortest shot |
| `CINEMATIC_SPAWN_TIME` | 0.5 | Time after a spawn that cuts at once (unchanged) |
| `CINEMATIC_SIGHT_WIDTH` | 0.2 | How far either side of the player it looks |
| `CINEMATIC_THREAT_HOLD` | 1.25 | Time held after a hit, in the game's time |
| `CINEMATIC_THREAT_EDGE` | 0.8 | How near the screen's edge the two are kept |
| `CINEMATIC_THREAT_WIDEST` | 90 degrees | Widest view for a threat |
| `CINEMATIC_THREAT_FACED` | 30 degrees | Aim this near the attacker ends it |
| `CINEMATIC_TETHER_ARMS` | 3.5 | The maximum distance, in placed arms |
| `CINEMATIC_TETHER_SETTLE` | 1/3 | How much of that a chase comes in to |
| `CINEMATIC_TETHER_ARRIVED` | 0.1 | How near that is a tripod again |
| `CINEMATIC_TETHER_TIME` | 1.0 | The chase's spring, in the game's time |
| `CINEMATIC_TETHER_MOST_PULL` | 16.0 | Most it is pulled at once |
| `CINEMATIC_JUMP_LEAST` | 1.0 | Least distance that can be a jump |
| `CINEMATIC_JUMP_SPEED` | 60 | Speed that is a jump, units a second |
| `CINEMATIC_ORBIT_SPEED` | 1.0 | Across its view this fast starts the arc, units a second |
| `CINEMATIC_ORBIT_TURN` | 0.5 | Or turning this fast, radians a second |
| `CINEMATIC_ORBIT_AFTER` | 0.75 | For this long |
| `CINEMATIC_ORBIT_REST` | 0.5 | Neither, this long, stops it |
| `CINEMATIC_ORBIT_QUARTER` | 45 degrees | Rear quarter, either side of behind |
| `CINEMATIC_ORBIT_TIME` | 1.5 | Arc's spring |
| `CINEMATIC_ORBIT_LEAST` | 0.5 | Least distance across for the arc |

## Still to do

1. **Run it in the game.** A replay with a lot of close combat on a map that is
   both indoors and out covers most of it. Check:
   - that the repeated re-cuts near scenery and doors are gone;
   - tight corners and low ceilings, where it falls back to the crane or the side;
   - a valley or a trench, for whether the camera rises enough;
   - being shot from behind while far away. The jump from a narrow zoom to a
     wide view may look abrupt;
   - a long run in the open and a Warthog, for how the repeated chases look;
   - whether a player 12 units away is too far for the tripod, and is hidden from
     it, and cut away from, more often than before;
   - a fight during a chase, for whether the arc helps in the short time it has;
   - a player against a wall, and running through a doorway;
   - a teleporter, and a replay at 0.25x and 4x, and paused.
2. Tune the numbers above from what that shows.
3. Measure the rays on the Switch: a placement, and now the frames it moves in.
4. Merge to `main`.

The camera does not back away from a player who runs at it. Goal 1 of the addendum
would like that, but no item asks for it, so it has not been added. It can walk
through the camera, as they could the tripod.
