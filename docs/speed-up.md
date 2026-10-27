# Speed-up (fast-forward): design note

Status: **open, not started**. A plan for a speed-up setting in the Linux library front end (`frontend/nxasahi.py`), with a turbo key in the game window. Nothing here is implemented yet.

## What it would look like

- Preferences → Performance: **Speed limit** set to 100 %, 150 %, 200 %, 300 % or unlimited. A game's Properties can override it.
- In the game window: a key that turns turbo on while held or toggles it (for example Tab, as the hold key, or a key of its own in the Controllers window). The window title shows the current speed.
- The library would pass the speed to the host in an environment variable (for example `DM_SPEED=2.0`). The turbo key would be handled in `dm/host/linux/front_sdl.c`, which tells the host the new speed while the game runs.

## What has to change in the host

The game measures time through the host, so the speed-up belongs there and not in the game's code:

1. **Guest clock.** `guest_ns()` in `dm/host/hle_os.c` is the one source for the system counter the game reads (the rewritten `mrs cntpct` and `svc GetSystemTick`, `hle.c`). Speed `k` would make it run as `base + (now - base) * k`. When `k` changes, `base` moves so that the clock goes on from where it was and does not jump. This has to work with the clock freezes that already exist (`guest_clock_freeze_for`) and with `DM_DETCLOCK`.
2. **The vsync event.** `hle_sdk16.c` fires the display's vsync event at 60 Hz (`DM_VSYNC_HZ`) or on the display link. Games that pace their frames by this event would need it at `60 * k`.
3. **Presenting frames.** `dm/host/linux/vk_window.c` creates the swapchain with `VK_PRESENT_MODE_FIFO_KHR`, which holds presents to the display's refresh rate. Above 100 %, use MAILBOX or IMMEDIATE when the device offers them, or skip presenting some frames.
4. **Sound.** At speed `k` the game produces `k` times as much audio. Either drop the extra samples or speed up playback (`dm/host/linux/audiounit_sdl.c`) so that the audio buffer does not grow without bound. Games that time themselves by the audio renderer would also have to consume samples faster.
5. **Frame limits inside the game.** Some engines cap their own frame rate (`hle_fpslock.c`, profile `fps_target`). Such a game only gets faster if both its limit and the guest clock are changed. Check this per engine (Unity, RE Engine, Unreal).

## Risks to check

- Games that step their logic by the real time between frames (variable time step) only speed up through the guest clock. Games that step a fixed amount per frame only speed up if they produce more frames. Try one game of each kind.
- Timeouts and timers that the host runs on its own clock (`now_ns()`) and not the guest's would fall out of step with the game.
- Network or local-wireless code and the time of day (`hle_time.c`) should keep real time.

## Order of work

Start with the guest clock and the swapchain present mode (steps 1 and 3), test it on a Unity game such as Shining Pearl, then do the vsync event, the sound, and the front end's setting and key.
