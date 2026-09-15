# Homecoming — the Legionary jetpack arrival in the Military hangar: research notes

Scope: the moment in the shipped Military hangar beat where three Legionaries (the "fodder"
squads) arrive from behind the Cabal command ship with their jetpacks when the player reaches
the first explosion area. Everything below was established live against the Shadowkeep-era
client, by observing the game and correlating with server logs, plus static reading of the
client binary. The notes describe the mechanisms in general terms so they can be mapped onto
any server implementation.

## 1. What the shipped beat looks like

- The player enters the "escape / explosion A" area of the hangar.
- Three Legionaries come **from behind the command ship** (far from their authored anchors),
  flying in with jetpacks, and land in the fodder firing area in front of the player.
- Their squad names in the package: `sq_hangar_fodder_a`, `_b`, `_c`. They are plain
  Legionaries (same actor class as every other hangar Legionary).

## 2. What the package contains for these squads (and what it does not)

- Each fodder squad has its own spawner config with **one inline point**: its authored anchor,
  which is the *landing* spot in front of the player, not the ship.
- The three fodder spawner configs are **byte-identical** to the config of an ordinary hangar
  squad (`sq_hangar_a_a_flank`) except for the name, the anchor point and the name hash. So the
  package carries **no "arrive by jetpack" behaviour** on the squad itself.
- No authored placement exists behind the command ship except the drop-pod point used by
  another squad (`sq_hangar_a_b`). There is no authored path (type-58) anywhere in the hangar.
- Six hangar **spawn rules** (type-66 objects, `sr_*`) exist: ceiling melee, ceiling rear,
  phalanx, the `a_b` pod, the fodder pod, the hallway destruction. Each rule holds one point
  row. Two of them resolve to real authored placements (the two pods); the ceiling/phalanx
  identities exist in no placement table (dead rules). None describes a jetpack entry.
- A scene (`sc_explosion_a`) plays at that spot but has **no squad participant** (a point set
  only): it cannot be what brings the Legionaries.

Conclusion: the arrival is not authored as content. It has to come from the server telling the
client *where* to spawn the squad (a spawn origin different from the anchor) and from the
Legionary AI itself, which knows how to jet toward its objective.

## 3. The squad placement message: the two spawn references

The squad authority body (the message that places a squad) carries, besides the requested
counts and the authored profile, **two object references** that we call the spawn references,
right after the spawn generation and two unknown references:

- the first one selects a **spawn rule** (a type-66 `sr_*` object). Verified live: placing a
  squad with a rule that is a crashing drop pod makes it arrive in that pod. This is the real
  mechanism behind the shipped pods.
- the second one is still unexplained. Live results:
  - a type-66 rule in it is accepted (no freeze, sent five times) but changes nothing visible;
  - a **point set** (type-48) or a **nav point** (type-47) in it **freezes the client** (main
    loop stall within ~2 s). The reference type is not the issue by itself (the same kinds are
    accepted elsewhere); the client consumer of that field does something with the resolved
    handle that a point/nav resolution breaks. Read the native consumer before sending anything
    else there.

Everything else in that body (requested counts, profile lanes, objective reference, objective
revision, task group) is understood and used.

## 4. What does make a Legionary jet: the combat objective's task groups

The behaviour that *looks* like the shipped arrival was reproduced through the AI, not through
spawning:

- A squad gets its AI only when it is assigned a **combat objective** (an objective sensor
  object) **before** it is placed; without it every Cabal stands still. The assignment carries
  an objective reference, a revision and a **task group** index.
- Rules found live: an assignment sent to a living squad is ignored unless it changes the group
  at the same revision; the revision must be new on every mission run (the client remembers
  the last revision per squad across mission restarts within one process).
- The client publishes, per squad, a list of **task costs** (one per task group, saturated value
  = unreachable). The list arrives piecemeal and noisily; a sane policy is to wait ~1 s after
  the first costs, pick the cheapest reachable group, and keep it unless it becomes unreachable.
- The hangar objective's task groups map to combat areas + tactics. On a fodder Legionary:
  - some groups walk to the overlooks through the stairs;
  - some groups make the squad leave the streamed area (it despawns);
  - **groups 12/13/16 hold the explosion-A fodder firing area with jetpack hops**; 14 holds
    explosion B; 15 the overlook above B.
- A Legionary on **group 13, placed far from the player, jets toward him**: that is the
  jetpack traversal. So the AI has the move; what the shipped beat adds is a *distant spawn
  origin* (behind the ship) so that the traversal is visible.
- A live group change (13 → 14) on a living squad did not move them; group changes on a living
  squad are not a reliable way to "send them behind the ship and back".

## 5. Actor programs: what they can and cannot do here

The client accepts per-combatant "actor programs" (cell authority on the squad's type-2
combatant cell). Facts established live:

- A program **drives an actor only if the program created it**: sending a program to the cell
  of a squad that is *not placed* creates a docile actor that then obeys every program; an actor
  spawned by a squad placement ignores programs entirely (passive or fighting alike).
- Every action needs a **double send** (a single send does not take on a live actor).
- Actor states (14 for the Legionary class, all in one group; names never recovered, only
  hashes): most are static poses; one is a climb-and-drop traversal (up like a ladder, down
  with a sideways jump), one is "alert then combat", one a console interaction. **None is a
  jetpack jump.**
- Spatial references: the program handlers resolve **only a point set (type-48; the marker is
  the point index) or an authored path (type-58)**. Any other reference type leaves an invalid
  handle that the handler then uses as a table index → the client freezes. Even when accepted,
  a point set only makes the actor *face* the point; neither the action target, nor the path
  program (either value of its follow flag), nor the other decoded program kinds move a
  combatant.
- Of the ten program kinds, two are identified (path, action); the eight others are decoded to
  their wire shape but unnamed. A body of the wrong bit length is **not refused: it freezes the
  client**, so probing shapes costs one game restart per wrong guess. Strings next to the
  handler table ("couldn't find firing point", "reference frame deleted", "discard not making
  progress") suggest one kind targets a *firing point* (type-44 firing areas such as the fodder
  area exist).

Conclusion: actor programs are not the shipped jetpack mechanism either.

## 6. Where the investigation stands

Facts:
1. The jetpack move exists and is the Legionary AI holding/reaching its firing area (task group
   13 of the hangar objective) — no scripting needed for the move itself.
2. The shipped beat spawns the three Legionaries **somewhere behind the ship**, which the package
   does not describe as a placement; the only server-side lever with that shape is the
   **second spawn reference** of the squad placement message.
3. That second reference freezes the client with a point set or a nav point, and is inert with a
   spawn rule.

Open leads, in order of expected value:
- **Read the client consumer of the second spawn reference** (the code path that resolves it
  when the squad spawns). It decides which object type is legal there and what it does with the
  position; the freeze says the current guesses resolve to the wrong kind of object.
- Check whether the fodder squad's *authored profile lanes* (the four-lane profile carried by
  the placement message) select an alternate spawn origin — unexplored.
- The two unknown references just before the spawn references, and the unknown integer fields
  around the task group, have never been varied.
- Alternative that avoids the unknown field: place the fodder with a spawn rule whose point is
  behind the ship (a rule row can be authored server-side if the client accepts a runtime rule),
  and let group 13 bring them in. Untested.

What NOT to retry (already ruled out live): point set / nav point in the second spawn reference
(freeze); every hangar spawn rule on the fodder at their anchors (nothing, except the fodder pod
rule which brings a crashing pod); actor states as a jump; program spatial targets as
locomotion; group changes on a living squad.

## 7. Tooling that made this possible (for reproduction)

- A live command channel into the mission script (a file polled by the running script) to try
  a placement variant without restarting the mission.
- A per-squad state feed from the client (alive count, task costs) logged with timestamps.
- A read-only dump of the client's reflection registry to decode the message bodies by field
  offsets and widths, checked against the declared bit counts before any live send.
- A rule of thumb that saved the most time: **a wrong bit length or an unsupported reference
  type is not refused, it stalls the client's main loop** — validate lengths offline and never
  batch guesses.
