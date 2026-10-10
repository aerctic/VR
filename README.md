# VRLayer

VRLayer is a Linux layer that puts flat PC games, including Steam games running through Proton, into a VR headset.

It works with no game-specific mods. VRLayer intercepts the game's Vulkan presentation and captures each frame together with the depth buffer. A small OpenXR app then shows the game in your headset:

- **Theater mode** (works with every game): the game appears on a big curved screen, in **stereoscopic 3D rebuilt from its depth buffer**.
- **Immersive mode**: the game fills your field of view at its own FOV. **Your head turns the game camera** through a virtual mouse. The last frame stays fixed in the world until the next one arrives, so turning your head stays comfortable.
- Your **VR controllers act as an Xbox 360 controller**.
- **Per-game profiles** keyed by Steam AppID, which you can tune live from a CLI.

```
 ┌──────────── game process ─────────────┐        ┌──────── vrlayer-compositor ────────┐
 │ game ─► DXVK / VKD3D-Proton / Vulkan  │        │ FrameServer: import shared memory  │
 │            │                          │  unix  │ StereoRenderer: depth ─► L/R views │
 │   VK_LAYER_VRLAYER_capture (implicit) │ socket │ OpenXR: quad / cylinder layers     │
 │   • copy swapchain image on present   ├───────►│ head pose ─► uinput mouse          │
 │   • copy main depth buffer per frame  │  + fds │ controllers ─► uinput Xbox pad     │
 │   • exported VkDeviceMemory (0-copy)  │◄───────┤ release slot                       │
 └───────────────────────────────────────┘        └─────────────┬──────────────────────┘
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
| **hold** both stick clicks for 1.5 s   | toggle theater / immersive        |

### Live control

```sh
vrlayer-ctl status                       # game, resolution, transport, fps, mode
vrlayer-ctl recenter
vrlayer-ctl mode immersive               # theater | immersive | toggle
vrlayer-ctl 3d off
vrlayer-ctl set stereo.separation 0.02   # any key from profiles/default.ini
vrlayer-ctl set stereo.show_depth on     # check which depth buffer was picked
vrlayer-ctl save                         # write ~/.config/vrlayer/games/<appid>.ini
vrlayer-ctl dump
```

## Tuning a game

`profiles/default.ini` documents every setting. Here is the usual workflow:

1. **3D**: run `vrlayer-ctl set stereo.show_depth on`. You should see a smooth greyscale scene with near objects dark. If it looks inverted, set `stereo.depth_reversed` (it is normally detected automatically). If it is upside down, set `stereo.depth_flip_y true`. Then adjust `stereo.separation` (strength) and `stereo.convergence` (what sits on the screen plane) until the scene looks right.
2. **Immersive mode**: set `display.game_fov` to the game's horizontal FOV. Then turn your head and adjust `input.mouse_counts_per_degree` until the world stays still. Turn off mouse acceleration and smoothing in the game.
3. Run `vrlayer-ctl save`.

### Layer environment variables

| Variable                    | Effect                                                                     |
| --------------------------- | -------------------------------------------------------------------------- |
| `ENABLE_VRLAYER=1`          | Turn the layer on for this process (`vrlayer-run` sets it).                |
| `DISABLE_VRLAYER=1`         | Force it off.                                                              |
| `VRLAYER_DEPTH=0`           | Don't track or copy depth (no 3D, lowest overhead).                        |
| `VRLAYER_DEPTH_MIN_DRAWS=N` | Minimum draws in a pass for its depth to be copied (default 8).            |
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

**Immersive head look.** Each frame, the head's yaw and pitch change becomes relative mouse counts (`HeadMouse`, with sub-count remainders carried over) on a uinput mouse. The compositor tracks where the game camera *should* be looking from the counts it actually sent. Each new game frame is placed at that orientation, optionally delayed by `immersive_latency_ms`. As a result the image stays locked to the world while the game catches up.

## Testing

```sh
cmake --build build && (cd build && ctest --output-on-failure)
```

- `unit`: config/profile parsing, settings, head-mouse math, quaternion helpers, IPC fd passing. It also checks that the shipped `default.ini` matches the built-in defaults.
- `e2e_{gpu,cpu}_{renderpass,dynamic}`: runs `vrlayer_test_game` (a real Vulkan swapchain app on `VK_EXT_headless_surface`) through the Vulkan loader with the layer enabled, and with the Khronos validation layer when it is installed. It receives frames exactly as the compositor does and checks the exact color and depth values and the stereo parallax direction. This works on Mesa's lavapipe, so no GPU is needed.
- `xr_smoke` (needs `monado-service`): runs the full pipeline against Monado's simulated HMD and null compositor. It covers a game started before the compositor, live control, a game crash, the CPU transport, and saving and reloading a profile.

## Limitations

- Stereo is **synthesized from depth**, not rendered twice. The HUD and any transparent effects that don't write depth sit at screen depth, and depth edges can show small artifacts.
- The depth heuristic can pick the wrong buffer in some engines. `stereo.show_depth` shows what was picked, and `VRLAYER_DEPTH_MIN_DRAWS` can help. MSAA depth buffers aren't captured; turn MSAA off in-game, or use the game in 2D theater mode.
- Immersive mode turns the camera only (yaw/pitch). There is no positional 6DoF or roll, and it needs mouse-look calibration per game. Games with mouse acceleration or smoothing will drift.
- HDR swapchains are shown without tone mapping.
- Some anti-cheat systems may not like injected layers or virtual input devices. Be careful with online games.
- So far this has been validated on Mesa lavapipe with the Monado simulated HMD (the automated tests above). It has not yet been tested with real headsets or a large set of commercial games. Expect per-game tuning.

## Layout

```
common/       wire protocol, IPC (fd passing), INI config
layer/        Vulkan implicit layer (runs inside the game)
host/         frame import, stereo pass, settings, head mouse, uinput (no OpenXR)
compositor/   OpenXR app: session, controller mapping, main loop
shaders/      stereo compute shader
tools/        vrlayer-ctl, vrlayer-run
profiles/     default.ini and per-game profile docs
tests/        unit, end-to-end (layer + loader + lavapipe) and OpenXR smoke tests
```
