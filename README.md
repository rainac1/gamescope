## gamescope: the micro-compositor formerly known as steamcompmgr

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.

It also runs on top of a regular desktop, the 'nested' usecase steamcompmgr didn't support.

 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it.
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

It runs on Mesa + AMD or Intel, and could be made to run on other Mesa/DRM drivers with minimal work. AMD requires Mesa 20.3+, Intel requires Mesa 21.2+. For NVIDIA's proprietary driver, version 515.43.04+ is required (make sure the `nvidia-drm.modeset=1` kernel parameter is set).

If running RadeonSI clients with older cards (GFX8 and below), currently have to set `R600_DEBUG=nodcc`, or corruption will be observed until the stack picks up DRM modifiers support.

## Building

Dependent first (**Debian/Debian-based**):
```
apt install meson ninja-build pkg-config cmake libpipewire-0.3-dev hwdata libx11-dev libwayland-dev libvulkan-dev wayland-protocols libx11-xcb-dev libxdamage-dev libxcomposite-dev libxcursor-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev libxkbcommon-dev libcap-dev libsdl2-dev libavif-dev libpixman-1-dev liblcms2-dev libseat-dev libinput-dev xwayland libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-res0-dev glslang-tools libluajit-5.1-dev libcatch2-dev
```

Build with:

```
git submodule update --init
meson setup build/
ninja -C build/
build/src/gamescope -- <game>
```

Install with:

```
meson install -C build/ --skip-subprojects
```

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest neighbour filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + I** : Increase FSR sharpness by 1
* **Super + O** : Decrease FSR sharpness by 1
* **Super + S** : Take screenshot (currently goes to `/tmp/gamescope_$DATE.png`)
* **Super + G** : Toggle keyboard grab

## Examples

On any X11 or Wayland desktop, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -h 720 -H 1440 -S integer -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

## Options

See `gamescope --help` for a full list of options.

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Ignored in embedded mode. If `-H` is specified but `-W` isn't, a 16:9 aspect ratio is assumed. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling
* `-F sgsr`: use Snapdragon™ Game Super Resolution 1 with RCAS sharpening for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.

## Screen pan effect

The engine pitch is tracked at all times: every y mouse movement, pan key held or
not, moves `phi_pitch`, and it saturates at `--pan-effect-pitch-limit`
(`pan_effect_pitch_limit`, 89 degrees by default) the same way a game's own pitch
clamp does.

While the pan key is held, mouse motion accumulates into `(delta_x, delta_y)`
counts and the game's frame is translated in output pixels right before it is
presented, relative to the pitch the engine was at when the key went down
(`phi_0`):

```
theta_x = k * delta_x              (pan yaw, accumulated while the key is held)
theta_y = k * delta_y              (pan pitch, accumulated while the key is held)
phi     = phi_0 + theta_y          (engine pitch now)

         sin(theta_x) / cos(phi)
dX = f * ---------------------------------------------
         cos(theta_x) + tan(phi_0) * tan(phi)

         tan(phi) * cos(theta_x) - tan(phi_0)
dY = f * ---------------------------------------
         cos(theta_x) + tan(phi_0) * tan(phi)
```

The projection has a pole where that denominator vanishes (the two pitches small
and of opposite sign, yaw out towards 90 degrees). The yaw is held on the near
side of it, so the translation stays finite and does not flip sign.

Pixels pushed outside of the output are cropped and the vacated space is left
black. Releasing the key clears the accumulated motion, so the picture returns
to where it was. The tracked pitch is of course not cleared: it is where the
game's camera is looking.

* `k` is `--pan-effect-angle` / `pan_effect_angle`, in degrees per mouse count, `0.022` by default.
* `f` is `--pan-effect-focal` / `pan_effect_focal`, in pixels, `720` by default.
* The engine pitch limit is `--pan-effect-pitch-limit` / `pan_effect_pitch_limit`, in degrees, `89` by default.
* The key is `--pan-effect-key` / `pan_effect_key`: an xkb keysym name or a comma separated list. `Alt`, `Super`, `Shift`, `Control`, `Meta` and `Hyper` expand to both the left and right variants, and `Ctrl`/`Win` are accepted as aliases.
* A positive `delta_x` yaws the world right and a positive `delta_y` pitches it down. Use `--pan-effect-invert` / `pan_effect_invert` for the opposite direction.
* `--pan-effect-disable` or `pan_effect_enabled=0` turns the effect off.
* `pan_effect_debug=1` (no command line flag) prints the pan counts, the pitch
  before and after the hold, and the resulting translation on every press and
  release, which is the easiest way to calibrate `k` and `f`.

### Calibrating the tracked pitch

The tracked pitch is an estimate of where the game's camera is pointed, so it
starts out unknown. It is resynchronised by hand, which the pitch limit makes
exact: look all the way down and keep pulling the mouse down until the pitch
saturates, which pins it to the limit (looking all the way up pins it to minus
the limit). Do that before panning and the projection has a known `phi_0`.

### Pixel exactness

The translation is snapped to whole source texels, so the game's frame is moved
without being resampled: the pan itself introduces no interpolation blur, and a
layer that is already 1:1 with the output moves in whole output pixels. Holding
the key without moving, or with less than half a texel of motion, applies
nothing at all.

It is also clamped to half of the output in each direction, so the picture always
keeps covering a good part of the screen and can never leave it. Moving the base
layer out of the output completely is a state no backend can present any more
(nothing of the layer is visible, so a plane cannot carry it), and games can
stall when their frames stop reaching the display there. Motion that would push
the translation past those bounds is not accumulated either, so holding the mouse
against a limit and then reversing moves the picture back immediately instead of
first unwinding the excess.

While the effect is engaged the frame is always presented the same way, even on
the frames where the translation snaps away to nothing, so crossing a texel
boundary cannot make the presentation path flip back and forth. A 1:1 layer in a
nested session is then presented with no compositing at all: it is moved by its
own plane, with the black backing plane below it. Scaled layers, and DRM or
OpenVR sessions, keep going through the composite, which is still sampled at
whole source texels.

### Where the counts come from

Only relative motion is ever accumulated:

* **DRM / session mode** (`--backend drm`, or a gamescope session): libinput's
  counts, straight from the device.
* **Nested mode** (`gamescope -- ...` inside a desktop session): the host
  compositor's `zwp_relative_pointer_v1` deltas. They are unaccelerated and,
  unlike differencing absolute positions, they are not clipped when the host
  pointer reaches a screen edge. gamescope keeps a relative pointer object alive
  for this even for applications that never lock the pointer, and the relative
  pointer of the nested relative mouse mode feeds the same accumulator when it is
  engaged. A compositor that sends absolute motion without relative motion cannot
  drive the effect in nested mode.

Those same unclipped deltas are forwarded to clients in nested mode, together
with the absolute motion and in one `wl_pointer.frame`, the way the libinput path
does it. Otherwise an application that reads relative or raw mouse input (mouse
look) only sees the absolute position, which the host pins at a screen edge, and
stops turning there. `gamescope_wayland_mouse_forward_relative_motion=0` (or
`wayland_mouse_forward_relative_motion=0`) turns that off. `--force-grab-cursor`
is the other way to avoid the problem: it locks the host pointer so the position
is never clipped, at the cost of the host mouse being captured.

While the effect is active a scaled frame, or a DRM session, has to be composited
instead of scanned out directly, which costs the same extra latency as
`--force-composition`; a 1:1 layer in a nested session does not. The effect only
moves the picture: what the game receives from the mouse and keyboard is
unchanged.

### Setting the options

All of the options above are ordinary convars, reachable three ways:

* Command line, for the ones with a flag: `--pan-effect-key`, `--pan-effect-angle`,
  `--pan-effect-focal`, `--pan-effect-pitch-limit`, `--pan-effect-invert`,
  `--pan-effect-disable`.
* Environment variables, `gamescope_<convar>=<value>`. Booleans and numbers work
  as written (`gamescope_pan_effect_focal=2160`). **String values have to be
  quoted**, because the override is pasted into Lua verbatim:
  `gamescope_pan_effect_key='"Super_L"'`. An unquoted string becomes an undefined
  Lua global and silently blanks the option.
* At runtime, `gamescopectl <convar> <value>`, eg.
  `WAYLAND_DISPLAY=gamescope-0 gamescopectl pan_effect_focal 2160`. This goes
  through the convar callback, so it is also the way to raise a log channel
  (`gamescopectl log_pan_effect debug`); the `gamescope_log_*` environment
  variables cannot do that, as they assign the value without running the
  callback.

While tuning, a good workflow is to resynchronise the pitch first (see above),
then hold the key and move the mouse a fixed distance on the pad, and read the
numbers back:

```
$ gamescope_pan_effect_debug=true gamescope -- glxgears
[pan_effect] Pan hold started at pitch -89.0 deg.
[pan_effect] Pan hold released, recentring. yaw=812.0 counts -> trans=(60.1, 0.2) px
             (pitch -89.0 -> -89.0 deg, angle=0.0220 deg/count, focal=720.0 px)
```

`gain` is the small angle `f * k` in px/count. Raising `f` scales the shift
linearly and keeps the response smooth; raising `k` scales it the same but also
makes the `tan` saturate (and the picture go black) after `89/k` counts.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg?exclude_unsupported=1)](https://repology.org/project/gamescope/versions)
