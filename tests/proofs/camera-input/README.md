# Camera and input proof

Run this mod alone in a played level. Run it once at 30 and once at 60 frames per
second. Keep its log and screenshots.

Hold ZL to begin. The mod holds a look-at target on the captain, changes zoom,
then holds a different heading. It requests the Captain Hurt shake and stops
shakes. A successful request does not prove visible shake motion. It checks camera
restoration immediately after each release.

Capture the captain while the look-at hold is active. The log records the
captain's screen coordinates. Compare those coordinates with the screenshot.

After the camera phases, the mod adds X for two world steps. It checks the hold
while active and after expiry, records physical input separately, and measures
the captain's height. On Oatchi, X is Jump.

Then press Plus. The first press is hidden from the game and
remains visible to the mod. Wait for the log to say the hold was released, then
press Plus again. This press should open the map. The mod tries a one-step
synthetic Plus press to close it and checks expiry. You can also close the map
with the controller. The log distinguishes that closure from the synthetic test.

Keyboard input waits until the keyboard imports can be resolved. The input service
also has its own keyboard check. A requested frame limit is
not a frame-rate measurement. Keep the presenter's observed pictures, world
steps, and timings for each run.
