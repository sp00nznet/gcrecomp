# Gameplay findings: playing forward from the classroom (Tales of Symphonia, GQSF)

Harness: `tools/run-scripted.sh` with the segment scripts of `tools/input-scripts/` run in order
(`--script tos-new-game.txt --script tos-classroom.txt --script tos-first-walk.txt`), blank card, `GCRECOMP_AUDIO=null`.
Every claim below comes from a run of the recompiled game with screenshots looked at (none are committed).

## Reached, with the script that does it

| Segment | Script | State reached | Verified |
| --- | --- | --- | --- |
| blank card to the first dialogue | `tos-new-game.txt` | Lloyd asleep in class | screenshot |
| the classroom cutscene | `tos-classroom.txt` | Raine's lecture, Colette, Lloyd's lines (French text), then `lloyd_ex.bin` / `npc_all.bin` / `bgm_t001.song` load and the player controls Lloyd (about 600 s of real time at 60 Hz with A every 45 frames) | screenshots every 10 s |
| first steps | `tos-first-walk.txt` | the main stick (`stick`, new in the script language) walks Lloyd; the camera follows and changes angle; characters animate | 4 screenshots |

## Blockers and observations

| Symptom | Cause | Fix | How verified |
| --- | --- | --- | --- |
| The script language could not walk | `Script::step` only returned buttons | `stick` / `cstick` commands (SDK units or `up+left`), `Script::sticks()`, fed to port 1 in `input.cpp`; test in `tests/input_script_test.cpp` | unit test; Lloyd walks in the game |
| Two shots 10 s apart show the same dialogue ("Colette, toi qui es l'Elue...") while A is pressed every 45 frames: looked like a stall | not a stall: the scene is paced by its own timing (the line stays until its voice / animation ends); the next run went through at the same point | none needed | second run: same line, then Raine "Tout...", the end of the lecture, control of Lloyd |
| The box of a dialogue line is a translucent rectangle across Colette's chest in one shot | the dialogue window closing (shrinking) between two lines | none | the frames before and after show the box |
| START and X do nothing in the classroom, A next to Genis / Colette does nothing | not understood: either no menu / no talk target in this part of the prologue (the real game only opens the menu once the event allows it), or Lloyd is not close enough to a target. Walking works, so the pad is read | open: the next segment has to walk Lloyd to Raine (back right of the room) and press A, with a shot per step | 3 shots after X / D / A, identical |
| Card: what does the game write? | on a blank card the game formats it (`CARDFormat`), mounts it, creates `TOS_999` (1 block) and deletes it again (free-space / write test), leaving a formatted card without files | none: the card emulation does what the game asks | `[CARD]` log lines; `tools/card-check.py` on the image: header, both directories and both block maps pass; the older map copy (update counter 1) still has the block of `TOS_999` allocated, the current one (counter 2) has all 251 blocks free |
| Unresolved calls (`[FuncTable] Unresolved call`) | none seen in about 35 000 frames from boot to the classroom control | nothing to add to `GQSF.extra_funcs.txt` | `grep -c Unresolved run.log` is 0 |

## Not done (and why)

* **Save and load**: the game cannot be asked to save in the classroom (the menu does not open, see above), so
  `CARDCreate` / `CARDWrite` of a real save and loading it back with `--keep-card` were not exercised. The card
  emulation has unit tests (`card_image_test`, `card_hle_test`); `tools/card-check.py` is the independent check
  to run on the image once a save exists.
* **A battle**: the first fight is several scenes after the classroom; getting there blind costs about 12 minutes of
  real time per try (the whole cutscene runs at 60 Hz, there is no fast-forward), and the machine was shared. The
  GP features the fight needs (indirect texturing, fog, z-compare) are therefore not looked at here.
* **Speed**: the run is paced by the VI retrace and the guest timebase; a time-scale switch would shorten the
  iteration loop but is a change of the timebase model, not done here.
