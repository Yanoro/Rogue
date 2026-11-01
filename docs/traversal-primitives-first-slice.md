# Traversal primitives: first slice

**Status:** implemented, 2026-10-31, in the single navigator. `follow`,
`follow:<direction>`, `back`, `to:<n>` and `to:<candidate-key>` are accepted
moves. The attention gate from
[navigation-attention-gate.md](navigation-attention-gate.md) applies to every
primitive leg.

This is a deliberately small slice of
[traversal-primitives-design.md](traversal-primitives-design.md): it adds
commitments, but not `straight`/`continue`, `explore`, crossing place
transitions, or multi-leg route history.

## 1. Why fixed paths

A `follow` or `back` is a promise about *which cells* the character will walk,
not only where it will end up. If the engine only gave `MoveToCellAction` a
target, A* could cut across open ground and the primitive would no longer mean
what the model chose. Both resolvers therefore return two things:

- a `NavigationPlan::Candidate` (target, label, distance), and
- the exact `std::vector<GamePosition>` path the leg should walk.

`MoveToCellAction` gained an optional planned path (and its origin). It uses that
path instead of A*; a normal candidate still uses A*. The gate's pause/resume
continues the same fixed path. If the character was moved off the fixed path (a
wake, a teleport), the action discards it and re-plans with A* rather than
walking backward to the old start.

## 2. `follow`

**Grammar.** `follow` or `follow:<direction>` (`east`, `north-east`, `ne`, ...).
Without a direction it uses the character's facing.

**Preconditions.** The character must be standing on a **linear** place
(`PlaceQuery::IsLinearPlace`: a road, river, wall, thin ribbon). If not, the move
is rejected with a reason and the model can use `here:<direction>`.

**Semantics.**

- The first step must continue in the chosen/facing direction; otherwise the
  move is rejected.
- After the first step, the walk follows the **same place** through forced turns.
- It stops at:
  - a fork inside the place (more than one same-place continuation),
  - a junction with a **different linear place** (a side road, crossing, another
    walkable ribbon),
  - the end of the known/observed place,
  - a blocked or occupied continuation,
  - the 256-step safety cap.
- The target is the last cell before that stop; the model gets control there.

The path is confined to the current place. **Crossing a place transition is a
new decision for now** (for example a vertical road place ending at a horizontal
road place): the follow stops at the junction. This is the main limitation
relative to the design's "follow through a place transition when the surface is
unchanged"; it is deliberate until a real run shows whether the extra decision
matters.

The stop rule uses the current place only, so walls/water beside a road do not
count as branches (they are occupied or non-walkable); a walkable linear side
feature does.

## 3. `back`

**Grammar.** `back`.

**Semantics.** Retrace the **last completed leg** exactly, cell for cell, back to
the cell where that leg started (`legStart`). One leg only.

The engine stores `lastLegPath` when a leg completes. A gate `stop` stores the
walked prefix instead, so `back` after a gate returns to where that leg started.
The path is reversed and given to `MoveToCellAction` as a fixed path; it does not
A* around the leg.

`back` is rejected when:
- there is no stored leg path,
- the stored path does not end at the current cell,
- the last leg started at the current cell (nothing to undo).

After a second `back` leg completes, its own path becomes `lastLegPath`, so
repeated `back` is not an arbitrary route undo; it is "one leg at a time". A
route-history stack (`back:2`, `back:junction`, `back:start`) is future work.

## 4. `to:`

**Grammar.** `to:<n>` where `n` is the 1-based `MAP TARGETS` number printed in
the prompt, or `to:<candidate-key>` (`to:p12`, `to:here:north`).

**Semantics.** Walk to the target of the offered candidate. This is the same
target the candidate's own key would use; `to:` is the convenient/visible
spelling. It does **not** examine or use an object even if the candidate was an
examine candidate: `to:` only walks. Use `examine:<alias>` for the interaction.

`to:` is rejected if the number is out of range or the key is not offered. It
does not add a new target type or a coordinate; it is a reference to the current
decision's candidate set.

## 5. Prompt and validation

The single system prompt opens with **Two ways to move**: an ACTIONS key is one
leg, while primitives are commitments. It explicitly says that a `here:<dir>` on
a linear place is only a compass run (often six tiles), not the follow primitive,
and that a repeated ACTIONS line such as `here:east "walk an unnamed road east
for 6 tiles"` is the case for `follow:east`.

In the single navigator, the ordinary `here:<dir>` description carries the hint
at the decision point:

```
here:east  walk an unnamed road east for 6 tiles (compass run;
           follow:east goes to the next junction/end) (path 100%, road 100%)
```

The user prompt also prints a **PRIMITIVES** block immediately after the
ACTIONS/OBJECTS state, before `PREVIOUS PLAN`:

```
PRIMITIVES (commitments; use these when an ACTIONS line repeats the same feature):
  follow / follow:<direction>   follow the linear place underfoot ... to the next fork, junction or end; follow uses your facing
  back                          retrace the last leg to where it started
  to:<n>                        walk to MAP TARGETS number n; to:<key> also works
  A here:<dir> ACTIONS line is one compass run, not the follow primitive.
```

The final `MOVE:` line repeats the primitive syntax. Invalid primitive arguing
(no linear place, wrong direction, no last leg, bad number) is a normal retry:
the engine does not guess a target.

## 6. Interaction with the gate

`StartLeg` calls `BeginGateTracking` for every leg, including follow/back/to.
The gate can pause the leg; on `note`/`ignore`/timeout it resumes the **same**
fixed path. On `stop` it records the walked prefix in the chunk journey and hands
back. The chunk trace therefore shows the cells the primitive actually walked,
not an A* shortcut.

## 7. Verification

- `build/AIRogue` builds green (19:50).
- A standalone `NavigationPlan` test checks:
  - direction parsing (`NE`, `north-west`, unknown);
  - a 10-cell road: `follow` reaches the far end (9-cell path) with and without
    an explicit direction, and from the middle the facing chooses the forward
    half;
  - a wrong direction is rejected;
  - a side road makes `follow` stop before the junction (4-cell path);
  - a Y-shaped place is refused at the start rather than silently picking a
    branch;
  - `back` reverses a 9-cell leg to its start;
  - `back` with no movement is rejected;
  - the ordinary `here:east` description carries the `follow:east` hint.
- A standalone prompt test checks the **Two ways to move** system section, the
  `PRIMITIVES` block after the ACTIONS state, and the final `MOVE:` reminder.
- The 2026-10-31 19:36 maze run used 41 recorded moves and none was a primitive;
  it repeatedly took the 6-tile `here:east` compass run along the same road.
  That is the case the prompt and the per-action hint now target.
- Runtime behaviour (does the model choose follow/back well, are the stops at
  the right junctions, does the fixed path survive the gate) still needs a real
  map run after this prompt change.

## 8. Not in this slice

- `straight` / `continue` / `onward` (heading-relative, featureless);
- `explore`;
- following across a place transition;
- `back:<n>`, `back:junction`, `back:start`;
- `to` an arbitrary observed cell not in the candidate set;
- the two-context navigator's primitives (the two-context path still uses its
  candidate/monitor system).
