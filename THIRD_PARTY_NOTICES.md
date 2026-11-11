# Third-party notices

Third-party components keep their own licenses:

- `proxy/external/openvr/openvr.h`: Copyright (c) 2015 Valve Corporation, BSD 3-Clause
  (see `proxy/external/openvr/LICENSE`). Included unchanged.
- `plugin/external/commonlibf4` (git submodule): CommonLibF4 by Ryan McKenzie, MIT
  (see the submodule's LICENSE). This project applies `patches/commonlibf4-winapi-msvc-14.44.patch`.
- `plugin/src/main.cpp` reproduces the `F4SEPluginVersionData` struct layout from F4SE's public
  `PluginAPI.h` (F4SE by Ian Patterson, Stephen Abel and Expired) so that F4SE 0.7.x loads the plugin.
  F4SE itself is not included.
- FRIK (Fallout 4 VR Body, https://github.com/rollingrock/Fallout-4-VR-Body) was used as a reading
  reference only. No code from it is included.

Fallout 4 is a trademark of Bethesda Softworks / ZeniMax Media. This project contains
no game files and is not affiliated with or endorsed by Bethesda.
