# VRLayer

VRLayer is a Linux layer that puts flat PC games, including Steam games running through Proton, into a VR headset.

It works with no game-specific mods. VRLayer intercepts the game's Vulkan presentation and captures each frame together with the depth buffer. It also finds the game's **camera** in GPU memory. A small OpenXR app then shows the game in your headset:

- **VR loading room**: while the game starts you stand in a virtual room with a live preview of the game and a status panel. VRLayer analyses the game there: depth buffer, camera lens (FOV, near/far), camera position, mouse-look calibration, hands and controllers. Then it starts the best mode for that game by itself.
- **VR mode (6DOF, experimental)**: your head pose is written into the game's own view matrix. You look around and lean in the game world, and each frame is shown from the exact pose it was rendered with. Stereo comes either from the depth buffer or from alternate-eye rendering.
- **Theater mode** (works with every game): the game appears on a big curved screen in the room, in **stereoscopic 3D rebuilt from its depth buffer**.
- **Immersive mode**: the game fills your field of view at its own FOV. **Your head turns the game camera** through a virtual mouse.
- **Controls**: VR controllers act as an Xbox 360 controller, or as keyboard + mouse with snap turning. **Hand tracking** turns pinches into buttons. A **laser pointer** (controller or hand) moves the mouse on the screen and clicks.
- **Per-game profiles** keyed by Steam AppID. Detection results are saved automatically, and you can tune everything live from a CLI.

```
 ┌──────────── game process ─────────────┐         ┌──────── vrlayer-compositor ─────────┐
 │ game ─► DXVK / VKD3D-Proton / Vulkan  │         │ FrameServer: import shared memory   │
 │            │                          │  unix   │ loading room + status panel         │
 │   VK_LAYER_VRLAYER_capture (implicit) │ socket  │ StereoRenderer: depth ─► L/R views  │
 │   • copy swapchain image + depth      ├────────►│ OpenXR projection / quad / cylinder │
 │   • find camera matrices in memory    │ frames, │ mouse calibration, head mouse       │
 │   • write head pose into the camera   │ camera  │ hands, pointer, gamepad / keyboard  │
 │   • exported VkDeviceMemory (0-copy)  │◄────────┤ head pose, release slot             │
 └───────────────────────────────────────┘         └─────────────┬───────────────────────┘
                                                               ▼
                                          OpenXR runtime (Monado, WiVRn, SteamVR, ...)
```

## Which games work

| Game API                                   | Path through VRLayer                                                        |
| ------------------------------------------ | --------------------------------------------------------------------------- |
| D3D9 / D3D10 / D3D11 under Proton          | DXVK, then Vulkan. Supported.                                               |
| D3D12 under Proton                         | VKD3D-Proton, then Vulkan. Supported.                                       |
| Native Linux Vulkan games                  | Supported.                                                                  |
| Native OpenGL games                        | Run them on Mesa's Zink (`MESA_LOADER_DRIVER_OVERRIDE=zink`), then Vulkan.  |
| Proton games using WineD3D (`PROTON_USE_WINED3D=1`) | Not supported. Use DXVK, the default.                              |

32-bit and 64-bit games both work: build the layer for both architectures (see below).

## Building

Dependencies (Debian/Ubuntu names): `cmake g++ libvulkan-dev glslang-tools libopenxr-dev pkg-config`. Add `g++-multilib` for the 32-bit layer.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j
cmake --install build

# 32-bit layer for 32-bit games (layer only, no compositor):
cmake -S . -B build32 -DCMAKE_INSTALL_PREFIX=$HOME/.local -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32 \
      -DCMAKE_INSTALL_LIBDIR=lib32 -DVRLAYER_BUILD_COMPOSITOR=OFF -DVRLAYER_BUILD_TESTS=OFF
cmake --build build32 -j && cmake --install build32
```

The install puts these in place:

- `lib/vrlayer/libVkLayer_vrlayer.so` and `share/vulkan/implicit_layer.d/VkLayer_vrlayer.<arch>.json`. The layer is dormant unless `ENABLE_VRLAYER=1` is set.
- `bin/vrlayer-compositor`, `bin/vrlayer-ctl` and `bin/vrlayer-run`
- `share/vrlayer/profiles/default.ini`

For head tracking and the gamepad, your user needs access to `/dev/uinput`. Steam's `steam-devices` package usually grants it. Otherwise:

```sh
sudo cp packaging/70-vrlayer-uinput.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger --name-match=uinput
```

## Using it

1. Start your OpenXR runtime (Monado/`monado-service`, WiVRn, ALVR, SteamVR, ...) and put the headset on.
2. In Steam, open the game's **Properties → Launch Options** and enter:
   ```
   vrlayer-run %command%
   ```
3. Launch the game. `vrlayer-run` starts `vrlayer-compositor` if it isn't already running and enables the layer for that game only. The compositor's log goes to `~/.local/state/vrlayer/compositor.log`.

Outside Steam, use `vrlayer-run ./my-game`, or run `vrlayer-compositor` yourself and start the game with `ENABLE_VRLAYER=1`.

### The VR loading room

When a game connects, you stand in a virtual room. A live preview of the game sits in front of you, and a status panel sits at hand height. **Load a save or otherwise get into 3D gameplay.** The camera can only be found while the game is rendering it. You can use the game's menus with your controllers, or point at the preview with the laser and click with the trigger.

The panel ticks off what VRLayer has learned:

| Check | How it is found |
| ----- | --------------- |
| Game video | Frames arrive through the layer (zero-copy or CPU copy). |
| Depth buffer | The full-screen depth attachment with the most draws. |
| Camera lens | The game's projection matrix, found in its mapped constant buffers, push constants or inline buffer updates. This gives the exact FOV, near/far planes and the depth convention. |
| Camera position | The view matrix stored next to the projection, plus view-projection and inverse view when present. This makes 6DOF possible. |
| Gameplay | The camera must follow *your* input. VRLayer turns it with a small mouse movement, or holds the virtual right stick if the mouse does nothing, and watches it, then turns it back. Stand still for a second. A main menu's 3D background, or a cutscene, also has a camera, but it ignores input, so it is never used. This check also measures mouse look (counts per degree). |
| Hands & controls | Tracked controllers and hands, and the control scheme in use. |

**Main menus.** Many games render a 3D scene behind the main menu. VRLayer may find that camera first, but the gameplay check rejects it ("this camera ignores your controls") and retries every few seconds. When you load a save, the menu stops updating its camera while the level's camera is written every frame. The layer prefers cameras that are still being written over stale copies left in memory, so it switches by itself. If it still looks stuck, press **RESCAN** (or run `vrlayer-ctl rescan`). If the camera changes during play (you quit to the menu, or load another level), VR mode falls back to the screen until the new camera passes the check.

It then recommends **VR** (camera position found), **Immersive** (lens found and mouse look calibrated) or **Theater**, and starts it after a 5-second countdown, but only once gameplay is confirmed. Without `/dev/uinput` access the check can't run, so press **START** when you are in the game. If nothing is confirmed before `loading.timeout`, it starts in theater mode, never in VR. You can also click **START** or **THEATER** with the laser, or pinch on them. The results are saved to `~/.config/vrlayer/games/<appid>.auto.ini`, so the next launch skips the calibration. If nothing is found within `loading.timeout`, it starts in theater mode. Hold **Start** for 3 s at any time to return to the loading room.

### VR mode (6DOF)

On every frame the compositor sends your head pose to the layer. Just before the game's `vkQueueSubmit`, the layer rewrites every fresh copy of the game's view matrix with that pose (and the matching view-projection and inverse view). The game renders from where your head is, and the compositor shows that frame as an OpenXR projection layer from the exact pose it was rendered with, using the game's real FOV. The runtime's reprojection then keeps it locked to the world.

- `vr.camera = head` (default): every frame uses your head pose. Each eye gets its view through stereo rebuilt from depth, with depth now linearized exactly from the detected projection.
- `vr.camera = alternate`: frames alternate between the left and right eye positions. This is true stereo, but each eye runs at half the game's frame rate.
- `vr.aim = auto|head|body`: with `head` (the `auto` choice once mouse look is calibrated), the game's own aim follows your head through the virtual mouse, and only the remainder is injected. The crosshair stays where you look. With `body`, the head only moves the view and the game aims as before.
- Turning: the game's own stick turning, or snap turns with the right stick in the `keyboard_mouse` scheme, rotate the world around you like in native VR games.
- `vr.units_per_metre`: the game's world scale (Unreal Engine uses 100), so leaning moves the right distance.

### Controllers

| VR controller                          | Gamepad                           |
| -------------------------------------- | --------------------------------- |
| sticks, triggers                       | sticks, triggers                  |
| grips                                  | LB / RB                           |
| A / B (right), X / Y (left)            | A / B / X / Y (Index: left A/B = X/Y) |
| stick clicks                           | L3 / R3                           |
| left menu (Index: left trackpad press) | Start                             |
| left menu + left grip (Index: right trackpad press) | Back                 |
| **hold** left menu for 1 s             | recenter the screen               |
| **hold** left menu for 3 s             | back to the VR loading room       |
| **hold** both stick clicks for 1.5 s   | cycle theater / immersive / VR    |
| point at the screen or panel           | laser pointer; the trigger clicks (mouse on the game screen) |

**Hand tracking** (when the runtime supports `XR_EXT_hand_tracking` and no controller is in that hand):

| Gesture             | Action                                         |
| ------------------- | ---------------------------------------------- |
| index pinch         | trigger, and click while pointing (right: RT, left: LT) |
| middle-finger pinch | right hand: A, left hand: B                    |
| point with the hand | laser pointer from the index knuckle           |

Your hands, controllers and the laser are drawn in the room, on top of the game.

**Keyboard + mouse scheme** (`input.scheme = keyboard_mouse`, for games without controller support): the left stick is W A S D, the right stick is mouse look (snap turn in VR mode), the triggers are the mouse buttons, A is space, B is C, X is R, Y is E, LB is Q, RB is F, L3 is shift, Start is Esc and Back is Tab. Rebind any of them in the profile's `[bindings]` section.

### Live control

```sh
vrlayer-ctl status                       # game, phase, analysis, camera, mode, input
vrlayer-ctl continue                     # leave the loading room now (or: continue vr|immersive|theater)
vrlayer-ctl loading reanalyze            # back to the loading room and search the camera again
vrlayer-ctl rescan                       # forget the camera (e.g. a menu's) and search again
vrlayer-ctl gameplay                     # "I am in gameplay": trust the camera found right now
vrlayer-ctl calibrate                    # measure mouse look again
vrlayer-ctl recenter
vrlayer-ctl mode vr                      # theater | immersive | vr | toggle
vrlayer-ctl set vr.camera alternate
vrlayer-ctl 3d off
vrlayer-ctl set stereo.separation 0.02   # any key from profiles/default.ini
vrlayer-ctl set stereo.show_depth on     # check which depth buffer was picked
vrlayer-ctl save                         # write ~/.config/vrlayer/games/<appid>.ini
vrlayer-ctl dump
```

## Tuning a game

`profiles/default.ini` documents every setting. Here is the usual workflow:

1. **VR mode**: set `vr.units_per_metre` to the game's scale. If the world seems to move with you, try `vr.aim body`.
2. **3D**: run `vrlayer-ctl set stereo.show_depth on`. You should see a smooth greyscale scene with near objects dark. If it looks inverted, set `stereo.depth_reversed` (it is normally detected automatically). If it is upside down, set `stereo.depth_flip_y true`. Then adjust `stereo.separation` (strength) and `stereo.convergence` (what sits on the screen plane) until the scene looks right.
   When the camera lens was detected, depth is exact and only `stereo.separation` and `stereo.convergence_near` matter.
3. **Immersive mode**: `display.game_fov` and `input.mouse_counts_per_degree` are detected on the loading screen. If the world drifts when you turn your head, adjust the counts. Turn off mouse acceleration and smoothing in the game.
4. Run `vrlayer-ctl save`.

### Layer environment variables

| Variable                    | Effect                                                                     |
| --------------------------- | -------------------------------------------------------------------------- |
| `ENABLE_VRLAYER=1`          | Turn the layer on for this process (`vrlayer-run` sets it).                |
| `DISABLE_VRLAYER=1`         | Force it off.                                                              |
| `VRLAYER_DEPTH=0`           | Don't track or copy depth (no 3D, lowest overhead).                        |
| `VRLAYER_DEPTH_MIN_DRAWS=N` | Minimum draws in a pass for its depth to be copied (default 8).            |
| `VRLAYER_ANALYZE=0`         | No camera search or injection (no memory tracking hooks at all).           |
| `VRLAYER_INJECT_WINDOW_KB=N`| How far past the last camera copy to look for new ones at each submit (default 2048). |
| `VRLAYER_TRANSPORT=cpu`     | Force the CPU copy path. Automatic when the game and headset run on different GPUs. |
| `VRLAYER_GPU_SHARING=0`     | Don't enable `VK_KHR_external_memory_fd` on the game's device.             |
| `VRLAYER_SOCKET=name`       | Socket name (abstract namespace, or a path starting with `/`).             |
| `VRLAYER_DEBUG=1`           | Verbose logs.                                                              |

## How it works

**Capture layer** (`layer/`). This is a Vulkan implicit layer.

- At `vkCreateSwapchainKHR` it adds `TRANSFER_SRC` to the swapchain usage. At `vkCreateDevice` it adds `VK_KHR_external_memory_fd`.
- It tracks images, views, render passes, framebuffers and dynamic rendering, and counts the draws issued into each depth attachment. On every present it picks the full-screen depth buffer with the most draws as the scene depth. Hysteresis keeps the choice stable.
- After each render pass on that buffer ends, the layer records a copy of it into a staging buffer, inside the game's own command buffer.
- At `vkQueuePresentKHR` it submits a copy of the swapchain image and the depth into one of three exported frame slots. The submission waits on the present's semaphores, and the present then waits on the copy. A worker thread waits for the copy's fence and then announces the frame.
- The game is never blocked. If the compositor is missing or holds all slots, the frame simply isn't exported.

**Transport.** If the game and the compositor run on the same GPU (matching device and driver UUIDs), slots are `VkDeviceMemory` exported as opaque fds and passed over a `SOCK_SEQPACKET` socket with `SCM_RIGHTS`, with queue-family ownership handed off to `VK_QUEUE_FAMILY_EXTERNAL`. That is zero copy. Otherwise frames go through memfd shared memory. The socket lives in the abstract namespace, so games inside Steam's pressure-vessel container can reach it.

**Stereo** (`shaders/stereo.comp`). This is depth-image-based rendering. Raw depth (D16, D24 or D32F, normal or reversed-Z) is linearized. For each output pixel the shader searches for the source texels that a parallax of `separation * (1 - convergence / depth)` maps onto that pixel. It keeps the nearest one so foreground objects occlude correctly, and fills disoccluded gaps from the background. The output is a side-by-side image submitted as two eye-specific quad or cylinder layers. Reprojection is left to the OpenXR runtime's compositor.

**Camera detection** (`layer/analyzer.cpp`, `common/camera_math.cpp`). The layer tracks every host-visible mapping of the game (`vkMapMemory`). A background thread scans those mappings at the 16-byte alignment of `float4x4` constants for perspective projection matrices. It accepts either storage order, Vulkan/D3D conventions, reversed-Z, infinite far planes and TAA jitter, but only when the aspect matches the swapchain. The most common one is the main camera. Around its copies the thread looks for a rigid transform at a consistent offset (the view matrix), and for P·V and V⁻¹. Push constants and `vkCmdUpdateBuffer` data are checked inline. When several cameras are found, the layer snapshots their constant blocks, lets a few frames render and compares. Cameras whose copies are still being written win over stale ones, such as a menu's camera left in memory after a level loads. Each camera gets an id, so the compositor sees switches. If the tracked camera stops being written for 3 seconds, a background search looks for a live one without interrupting tracking. Because a projection's bits don't change between frames, it serves as an anchor: each frame, the copies near the last one are found again and the view matrix is read from the learned offset. That gives the game's camera pose, which the mouse calibration watches.

**Injection.** In VR mode the layer computes `D` from the head pose. The pose is mapped into the game's camera axes, using the projection's forward direction and the sign of the y-axis, which depends on whether the game flips the viewport. At every `vkQueueSubmit` the layer replaces fresh view matrices `V` with `D⁻¹·V`, and keeps P·V and V⁻¹ consistent. It remembers what it wrote, so a copy is never patched twice, and the original camera is still reported. Each frame carries the pose sequence it was rendered with.

**Immersive head look.** Each frame, the head's yaw and pitch change becomes relative mouse counts (`HeadMouse`, with sub-count remainders carried over) on a uinput mouse. The compositor tracks where the game camera *should* be looking from the counts it actually sent. Each new game frame is placed at that orientation, optionally delayed by `immersive_latency_ms`. As a result the image stays locked to the world while the game catches up.

## Testing

```sh
cmake --build build && (cd build && ctest --output-on-failure)
```

- `unit`: config/profile parsing, settings, head-mouse math, quaternion helpers and IPC fd passing. It also covers projection/view matrix recognition (Vulkan, D3D reversed-Z infinite, OpenGL, jitter, and rejection of non-cameras), the head-to-camera basis conversion, mouse calibration against a simulated lagging game (including accelerated and unresponsive cameras), keyboard mapping and snap turn, the loading-screen decisions, the font, and wire-format sizes. It checks that the shipped `default.ini` matches the built-in defaults and does not pin auto-detected values.
- `render`: renders the loading room, the hands/laser overlay and the status panel on the GPU and checks them. With `VRLAYER_DUMP_DIR` set it writes the images.
- `e2e_camera_menu`: the test game first shows a main menu with its own orbiting 3D camera, then loads the level and leaves the menu's camera behind in memory. The layer must report the menu camera first and then switch to the gameplay camera, with a new id.
- `e2e_camera_{gpu,cpu}`: the test game uploads a camera block to a ring buffer like a DXVK game does, and its shader encodes the camera yaw it saw. The test checks the detected FOV, near/far and view matrix, and the reported camera pose. It also checks that an injected head yaw really changes the GPU-rendered pixels, that alternate-eye frames are tagged, that turning injection off restores the image, and that detection works again after a restart.
- `e2e_{gpu,cpu}_{renderpass,dynamic}`: runs `vrlayer_test_game` (a real Vulkan swapchain app on `VK_EXT_headless_surface`) through the Vulkan loader with the layer enabled, and with the Khronos validation layer when it is installed. It receives frames exactly as the compositor does and checks the exact color and depth values and the stereo parallax direction. This works on Mesa's lavapipe, so no GPU is needed.
- `xr_smoke` (needs `monado-service`): runs the full pipeline against Monado's simulated HMD and null compositor, with the shipped profiles loaded. It covers the loading room finding the camera, the auto-start into VR mode, live head-pose injection (head and alternate-eye), the saved detection profile, returning to the loading room and continuing in theater mode, a game started before the compositor, live control, a game crash, the CPU transport, and saving and reloading a profile.

## Limitations

- Stereo is **synthesized from depth**, not rendered twice. The HUD and any transparent effects that don't write depth sit at screen depth, and depth edges can show small artifacts.
- The depth heuristic can pick the wrong buffer in some engines. `stereo.show_depth` shows what was picked, and `VRLAYER_DEPTH_MIN_DRAWS` can help. MSAA depth buffers aren't captured; turn MSAA off in-game, or use the game in 2D theater mode.
- **VR mode is generic and therefore experimental.** It works when the game keeps its camera in a constant buffer as a separate view matrix next to the projection. Engines that only upload a combined view-projection, keep several camera copies (previous frame, shadows, reflections), or cull on the CPU with the original camera will show artifacts: objects popping at the screen edges, wrong shadows, or reflections that don't follow. In those games the loading room recommends a different mode, or use `vrlayer-ctl mode theater`. The game's physics, HUD and aim aren't changed beyond what the mouse can do: there are no motion-controlled hands *inside* the game.
- Camera search reads the game's mapped memory from a background thread. On games with huge write-combined heaps the first search can take several seconds, and injection costs a little CPU at every submit.
- Mouse calibration needs a game that turns its camera with the mouse, without acceleration or smoothing. The gameplay check briefly turns the camera when the loading room or a camera switch asks for it. Games whose camera doesn't respond to the mouse or the right stick (for example fixed-camera games) are never confirmed: press START to use them anyway.
- The detected near/far assume Vulkan's [0, 1] clip depth (DXVK, VKD3D-Proton and native Vulkan). OpenGL games through Zink may get the wrong exact depth: set `stereo.depth_reversed` to fall back to the heuristic.
- Immersive mode turns the camera only (yaw/pitch). Games with mouse acceleration or smoothing will drift.
- HDR swapchains are shown without tone mapping.
- Some anti-cheat systems may not like injected layers or virtual input devices. Be careful with online games.
- So far this has been validated on Mesa lavapipe with the Monado simulated HMD (the automated tests above). That includes the camera detection and injection against a DXVK-like test game, but not real headsets, real hand tracking or commercial games. Expect per-game tuning, and some games where only theater mode works.

## Layout

```
common/       wire protocol, IPC (fd passing), INI config, camera matrix math
layer/        Vulkan implicit layer: capture, camera analyzer + injection (runs inside the game)
host/         frame import, stereo pass, room/overlay renderer, loading screen, panel font,
              mouse calibration, controls mapping, settings, uinput (no OpenXR)
compositor/   OpenXR app: phases, display modes, hands, pointer, layers
shaders/      stereo and room/overlay compute shaders
tools/        vrlayer-ctl, vrlayer-run
profiles/     default.ini and per-game profile docs
tests/        unit, end-to-end (layer + loader + lavapipe) and OpenXR smoke tests
```
