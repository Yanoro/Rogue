# Movement-locked casting

A skill with `"canCastWhileMoving": false` forces the player to stop when the
cast begins. Movement after that cancels the cast.

## The held-input case

The important edge case is casting while already moving. If the player is holding
W at activation, simply stopping the character and then checking for velocity on
the next frame used to cancel the cast immediately, because the held key re-added
movement.

The cast now carries `Casting.movementInputArmed` and `Casting.allowSliding`:

1. At activation, the game captures whether a movement key is already down.
2. If it is, the cast starts **unarmed**. Movement commands are suppressed while
   unarmed, and the held key is ignored. The character remains still.
3. When all movement input returns to neutral, the cast becomes **armed**.
4. Once armed, any new movement input, clicked path or actual movement cancels
   the cast.

A clicked path is always treated as new movement, even while an old held key is
still down.

## Result

- Cast while walking, keep holding the key: the character stops and the cast
  proceeds.
- Release the key, then move: the cast cancels.
- Click a destination during the cast: the cast cancels.
- Cast while stationary, then move: the cast cancels immediately.

The check lives in `CastMovementCancelSystem`; the suppression is in
`Game::handleInput`; the latch is initialized in `UseSkill` and stored on
`Casting`.

## Sliding casts

A movement-locked skill can opt into sliding:

```json
"canCastWhileMoving": false,
"allowSliding": true
```

With sliding enabled, activation does **not** call the normal forced-stop path.
The character keeps its current velocity and acceleration and coasts under the
existing friction/physics. A held movement key from before the cast is still
latched until neutral, so a moving cast does not cancel itself.

While the cast is sliding:

- actual velocity does **not** cancel it;
- new WASD input after the latch releases cancels it;
- a clicked movement destination cancels it.

`allowSliding` is per skill and defaults to false, so existing movement-locked
skills keep the previous stop-and-cancel behavior.
