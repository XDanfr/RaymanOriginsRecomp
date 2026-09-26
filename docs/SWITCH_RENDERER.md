# Nintendo Switch renderer

## Starting point

Rayman Origins Recompiled already has a native Vulkan renderer.

That means the first Switch graphics milestone should be:

> get the existing renderer to present through Nintendo Switch Vulkan, not write a new renderer.

See [NATIVE_RENDERER.md](NATIVE_RENDERER.md).

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

Keep two classes of problems separate when debugging:

1. Vulkan API correctness
2. Switch/NVK driver behaviour

## Renderer milestones

1. Vulkan instance creation
2. physical device selection
3. VI surface creation
4. swapchain creation
5. clear screen
6. one Rayman renderer frame
7. menus
8. gameplay
9. movies
10. long-running stability

## Platform split

A small platform presentation interface is preferable to spreading Switch conditionals across renderer code.

Use the actual Rayman renderer structure when implementing this. The names below are architectural guidance, not a requirement to create empty files.

    presentation.h
    desktop_presentation.cpp
    android_presentation.cpp
    switch_presentation.cpp
