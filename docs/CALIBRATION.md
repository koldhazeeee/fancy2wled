# Phone auto-calibration

The board works out **which LEDs it controls and exactly where they are relative to the TV** by watching them with your
phone. Everything happens on your phone and the board; nothing is uploaded anywhere and no camera image ever leaves your phone.

The goal: **LEDs along the width of the TV are exact** (each follows the part of the picture right next to it), and **LEDs
past the edge of the TV smooth out** toward the picture's average colour, reaching it at the end of the strip, instead of
repeating the edge colour for metres.

## What you do

1. On the device that feeds the TV (the one plugged into the Sync Box) open `http://fancy2wled.local/screen` and make it
   full screen. This "calibration screen" turns the TV black, shows nine squares, then white, by itself.
2. Dim the room lights.
3. On your phone open the setup page and tap **Auto-calibrate**. The page opens over HTTPS (browsers only allow
   camera access on HTTPS). The board makes its own certificate, so your phone warns that the connection is not
   private. Continue anyway: *Advanced -> proceed* (Android) or *Show Details -> visit this website* (iPhone).
4. Tap **Start camera**, stand 2-4 m in front of the TV, hold the phone upright and steady (propped up is best), with the
   **whole TV and all the LEDs in view**, then tap **Start calibration**. It takes 2-3 minutes.
5. Check the picture (green outline and squares = the TV it measured, orange dots = where it saw the LEDs) and tap **Save**.
   *Preview rainbow* shows red on the TV's left edge through to violet on the right edge across your LEDs.

## What happens

1. **Squares measure the TV.** With the room dark the TV shows nine small squares at known positions. The centre of each
   square is found to a fraction of a pixel (glare around a bright area is symmetric, so unlike an outline it does not move
   the centre) and from the nine centres the exact mapping between the camera image and the picture is computed, including
   perspective if your phone is not square-on. The white flash that follows is only a cross-check; if the squares can't be
   used it falls back to the white outline and tells you it is less exact.
2. **Find the LEDs.** The board lights blocks of LEDs one at a time, with the TV black. For each block the phone subtracts the
   dark baseline and finds the centre of the glow. A coarse pass over the whole WLED device finds which LEDs the camera can see,
   a finer pass (about 40 blocks) maps them, and a third pass shifted by half a block doubles the density next to the TV. The
   baseline before and after the scan is blended to cancel the phone's auto-exposure drift.
3. **Positions.** If the strip is a straight line, the positions are replaced by the best-fit line through all the glows (averaging
   out measurement noise); otherwise they are smoothed with a short local fit that keeps corners. LEDs outside the camera's view
   are extrapolated along the line of the visible ones.
4. **The table.** Each LED gets: the two border zones it blends between (it takes its colour from the zones along the nearest
   edge of the picture), and a **distance beyond the TV** (0 for any LED lined up with the TV). The same rule covers a
   straight strip behind the TV, a loop around it, or anything in between, in any order or direction.

## Smoothing beyond the TV

The board treats the two ends of the strip separately: the LEDs before the part lined up with the TV, and the LEDs after it.
Each end eases (smoothstep) from the TV's edge colour toward the **average colour of the whole picture**, reaching it at the end
of its own tail. The setup page's **Smooth beyond the TV** slider sets how far that takes: 100% = the whole tail, 50% = twice as
fast, 0% = off (the end LEDs just repeat the edge colour). It is a live setting; you don't need to re-calibrate to change it.
A strip that hugs the TV (a loop around it) has no tail and is never smeared.

## Tips

* A dark room matters more than anything else. The LEDs and the TV must be much brighter than their surroundings.
* If some LEDs are out of the camera's view, move back, or enter the LED range by hand ("I know the range"). LEDs outside
  the view get extrapolated positions, which is fine for a straight strip but not for corners.
* The phone must be upright (not rotated) and roughly level so that "top" of the TV is the top of the picture.
* The squares are small: stand close enough that the TV is at least a third of the picture width.
* Re-run it any time you move the strip or the TV.

## If it fails

The page tells you what it could not see:

* *"did not see the squares appear"* / *"could only find N of the 9 squares"* - the calibration screen is not on the TV (wrong
  input, not full screen), the TV is too small in the picture, or something bright is in view.
* *"could not see any LEDs light up"* - the camera does not see the LEDs, the room is too bright, or WLED is not receiving
  DDP (WLED -> Config -> Sync Interfaces -> *Receive UDP realtime*).
* Warnings (auto-exposure, LEDs cut off by the edge of the view, extrapolated LEDs, squares vs outline mismatch) are listed
  under the result.

## Doing it by hand

Under *Your LEDs* on the setup page you can instead say where the LEDs are and how they run:

* **Above the TV** (ceiling / top of the wall): uses the top row of the picture's colour map. **Below the TV**: the bottom row.
  **Left of / right of the TV** (a vertical strip): the left / right column. For these, give the run (first LED, length), the part of
  it that covers the TV (the strip's own LED numbers where the TV starts and ends, e.g. 83 to 163) and which way the numbers go.
  LEDs before and after the TV part get the same smoothing as the phone calibration.
* **All the way around the TV**: start corner, direction, LEDs per side.

Changing the run or the TV part lights **red** LEDs where the TV part starts and ends and **blue** LEDs where the run starts and ends
(about 8 seconds after your last change). *Start rainbow test* shows red at the TV's left edge through violet at the right edge, and
every change applies live until you press Save. *Find my LEDs* lights a block you can slide along the strip, with buttons to use that spot as the start or end of the run or
of the TV.
