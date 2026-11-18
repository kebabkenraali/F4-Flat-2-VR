# How to Create a FOMOD Mod Package (Vortex/MO2 compatible)

**In this repo, just run:** `powershell -ExecutionPolicy Bypass -File package_fomod.ps1`
It builds `dist\F4_Flat_2_VR_<version>.zip` from the current builds, including the
FOMOD XML files and the Vortex override instructions below. The guide explains
what the script does.

## Overview

A FOMOD package is a ZIP file that contains:
1. `fomod/` folder with `info.xml` and `ModuleConfig.xml`
2. `files/` folder with the actual mod files

Vortex and Mod Organizer 2 read the XML files to know where to install each file.

## Step-by-step

### 1. Stage files

Create a staging folder with this structure:

```
staging/
├── fomod/
│   ├── info.xml
│   └── ModuleConfig.xml
└── files/
    ├── d3d11.dll                    → goes to game root
    ├── fo4stereo.ini                → goes to game root
    ├── F4SE/
    │   └── Plugins/
    │       └── fo4flat_probe.dll    → goes to Data/F4SE/Plugins/
```

### 2. Create info.xml

```xml
<?xml version="1.0" encoding="utf-8"?>
<ModInfo>
  <name>Mod Name</name>
  <version>1.0.0</version>
  <author>Author Name</author>
  <description>Mod description</description>
</ModInfo>
```

### 3. Create ModuleConfig.xml

```xml
<?xml version="1.0" encoding="utf-8"?>
<config>
  <moduleName>Mod Name</moduleName>
  <requiredInstallFiles>
    <file source="files\d3d11.dll" destination="" />
    <file source="files\fo4stereo.ini" destination="" />
    <file source="files\F4SE\Plugins\fo4flat_probe.dll" destination="Data\F4SE\Plugins" />
  </requiredInstallFiles>
</config>
```

**Key rules:**
- `destination=""` = game root (e.g., `Fallout 4\`)
- `destination="Data\F4SE\Plugins"` = `Fallout 4\Data\F4SE\Plugins\`
- `source` paths are relative to the ZIP root
- No namespace needed on `<config>` element
- No `moduleImage` element needed (optional)

### 4. Validate with XIMOD Architect

Download XIMOD Architect from: https://github.com/plplecuyer/ximod-architect/releases

```bash
ximod-architect.exe validate <staging-folder>
# Output: OK — the FOMOD conforms to the schema.
```

### 5. Package with XIMOD Architect

```bash
ximod-architect.exe package <staging-folder> -o output.zip
# Output: Packaged 7 file(s) into output.zip
```

**OR** package manually:

```powershell
Compress-Archive -Path "staging\*" -DestinationPath "output.zip"
```

### 6. Install in Vortex

1. Remove any old version of the mod from Vortex
2. Delete the staging folder: `D:\Vortex Mods\fallout4\<mod-name>\`
3. Add the new ZIP to Vortex

## Common Mistakes

1. **BOM (Byte Order Mark)**: PowerShell's `Set-Content` adds a BOM by default. Use `[System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))` instead.

2. **Stale cache**: Vortex caches extracted mods. Always remove the old mod and delete the staging folder before re-installing.

3. **Wrong destination**: `destination=""` means game root, NOT `Data\`. Files that need to go to `Fallout 4\` must have `destination=""`.

4. **Missing fomod folder**: The folder MUST be named `fomod` (lowercase). Vortex looks for this exact name.

5. **Backups included**: Don't include `fomod/backups/` in the ZIP. XIMOD Architect's `package` command excludes them automatically.

## XIMOD Architect CLI Commands

```bash
# Validate FOMOD XML
ximod-architect.exe validate <root>

# Package into distribution ZIP
ximod-architect.exe package <root> -o output.zip

# Extract and inspect existing mod
ximod-architect.exe extract mod.zip

# Simulate installation
ximod-architect.exe simulate <root>
```

Where `<root>` is the folder containing `fomod/` and `files/`.

## File Locations for F4 Flat 2 VR

| File in ZIP | Destination |
|-------------|-------------|
| `files/d3d11.dll` | `Fallout 4\d3d11.dll` |
| `files/fo4stereo.ini` | `Fallout 4\fo4stereo.ini` |
| `files/F4SE/Plugins/fo4flat_probe.dll` | `Fallout 4\Data\F4SE\Plugins\fo4flat_probe.dll` |
