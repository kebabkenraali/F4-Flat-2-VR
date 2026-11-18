# MO2 setup (Mod Organizer 2)

`F4Flat2VR_Dev` is the mod folder used with Mod Organizer 2. It contains only the MCM configuration and
its settings template. The game files (`d3d11.dll`, `fo4flat_probe.dll`, ESP, scripts and the patched
`HUDMenu.swf`) are built or copied from the project and must not be committed.

Install: copy `mo2/F4Flat2VR_Dev/` into MO2's `mods` folder, add the built `d3d11.dll`, the plugin DLL,
`fo4stereo.ini` and the scripts next to it, enable the mod, and start the game through the `F4SE`
executable (`f4se_loader.exe`) so the virtual file system is active.
