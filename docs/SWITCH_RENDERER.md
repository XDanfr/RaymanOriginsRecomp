# Nintendo Switch renderer

## Starting point

Rayman Origins Recompiled already has a native Vulkan renderer.

That means the first Switch graphics milestone should be:

> get the existing renderer to present through Nintendo Switch Vulkan, not write a new renderer.

See [NATIVE_RENDERER.md](NATIVE_RENDERER.md).

The Switch target now has a deliberately small presentation probe ahead of
the Vulkan port.  It releases the libnx text console after the first guest
`XE_SWAP`, creates a 1280x720 double-buffered libnx framebuffer, and displays
animated diagnostic colour bars.  It also hooks Rayman's high-level D3D
shader, draw, clear, resolve, and present entry points and records their
activity in `runtime.log`. Once the console is released, stdout is redirected
to that log so later diagnostics cannot call into libnx's retired software
console renderer. This validates VI ownership and the native-renderer
hook boundary on hardware; it does **not** render the game or replace the
Vulkan renderer.

## Proposed stack

    Rayman renderer
          |
        Vulkan
          |
         NVK
          |
    VK_NN_VI_SURFACE
          |
      Nintendo VI
          |
      Switch display

The recent XenonRecomp Switch ports UnleashedRecomp-NX and MarathonRecomp-NX use the Vulkan/NVK route.

## Areas to audit

### Instance extensions

The Switch presentation path needs the Nintendo VI surface extension in addition to the normal Vulkan instance setup.

The desktop/mobile surface creation code should therefore be isolated behind a small platform presentation interface.

### Physical device selection

Do not assume the desktop device-selection logic applies unchanged.

The Switch target should prefer the relevant Vulkan device and avoid desktop-oriented GPU/vendor heuristics where possible.

### Surface creation

The Switch path should use the Nintendo Switch Vulkan presentation mechanism and create the VI surface using the extension exposed by the Switch Vulkan stack.

### Swapchain

Audit:

- surface format selection
- present mode selection
- image count
- image layout transitions
- synchronization primitives
- framebuffer dimensions
- resize assumptions

### Shaders

The existing XenosRecomp/native-renderer shader pipeline should remain the source of truth.

Determine:

- which SPIR-V is generated ahead of time
- whether current SPIR-V is accepted by the Switch Vulkan driver
- whether additional compiler options are required
- whether shaders need a Switch-specific cache

Do not add a second shader language unless the existing generated Vulkan shaders prove incompatible.

## NVK

The relevant Switch ports use Mesa's NVK Vulkan driver.

Unlike libnx, a Switch NVK build is not currently supplied by this repository
or by the installed devkitPro toolchain.  The real renderer target will need a
compatible Switch Vulkan header set and static NVK library supplied explicitly
to CMake.  Keep that dependency external to the repository and do not commit
driver build products or proprietary game shader assets.

Keep two classes of problems separate when debugging:

1. Vulkan API correctness
2. Switch/NVK driver behaviour

## Renderer milestones

1. [x] VI ownership and software-framebuffer presentation probe
2. [x] high-level Rayman D3D hook activity audit
3. [ ] Vulkan/NVK dependency integrated
4. [ ] Vulkan instance creation
5. [ ] physical device selection
6. [ ] `VK_NN_vi_surface` creation
7. [ ] swapchain creation
8. [ ] Vulkan clear screen
9. [ ] one Rayman renderer frame
10. [ ] menus
11. [ ] gameplay
12. [ ] movies
13. [ ] long-running stability

## Platform split

A small platform presentation interface is preferable to spreading Switch conditionals across renderer code.

Use the actual Rayman renderer structure when implementing this. The names below are architectural guidance, not a requirement to create empty files.

    presentation.h
    desktop_presentation.cpp
    android_presentation.cpp
    switch_presentation.cpp
