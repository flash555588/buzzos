param(
    [string]$Output = "build/user/doom.elf"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $root
$doom = "src/user/third_party/doomgeneric/doomgeneric"
$build = "build/doom"
New-Item -ItemType Directory -Force $build | Out-Null

$names = @(
    "dummy", "am_map", "doomdef", "doomstat", "dstrings", "d_event", "d_items",
    "d_iwad", "d_loop", "d_main", "d_mode", "d_net", "f_finale", "f_wipe",
    "g_game", "hu_lib", "hu_stuff", "info", "i_cdmus", "i_endoom", "i_joystick",
    "i_scale", "i_sound", "i_system", "i_timer", "memio", "m_argv", "m_bbox",
    "m_cheat", "m_config", "m_controls", "m_fixed", "m_menu", "m_misc", "m_random",
    "p_ceilng", "p_doors", "p_enemy", "p_floor", "p_inter", "p_lights", "p_map",
    "p_maputl", "p_mobj", "p_plats", "p_pspr", "p_saveg", "p_setup", "p_sight",
    "p_spec", "p_switch", "p_telept", "p_tick", "p_user", "r_bsp", "r_data",
    "r_draw", "r_main", "r_plane", "r_segs", "r_sky", "r_things", "sha1", "sounds",
    "statdump", "st_lib", "st_stuff", "s_sound", "tables", "v_video", "wi_stuff",
    "w_checksum", "w_file", "w_main", "w_wad", "z_zone", "w_file_stdc", "i_input",
    "i_video", "doomgeneric"
)
$flags = @(
    "--target=i386-none-elf", "-std=c11", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-fno-pic", "-mno-sse", "-mno-mmx", "-mfpmath=387",
    "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
    "-Wno-unused-function", "-DNORMALUNIX", "-DLINUX", "-D__BUZZOS__", "-D_DEFAULT_SOURCE",
    "-DDOOMGENERIC_RESX=320", "-DDOOMGENERIC_RESY=200", "-DFEATURE_SOUND",
    "-Isrc/user/libc", "-Isrc/user/third_party/doomgeneric/doomgeneric"
)
$objects = @()
foreach ($name in $names) {
    $input = Join-Path $doom ($name + ".c")
    $object = Join-Path $build ($name + ".o")
    & clang @flags -c $input -o $object
    if ($LASTEXITCODE -ne 0) { throw "Doom compile failed: $input" }
    $objects += $object
}
$portObject = Join-Path $build "doom_buzzos.o"
& clang @flags -c "src/user/bin/doom.c" -o $portObject
if ($LASTEXITCODE -ne 0) { throw "Doom BuzzOS frontend compile failed" }
$objects += $portObject
$audioObject = Join-Path $build "doom_audio.o"
& clang @flags -c "src/user/bin/doom_audio.c" -o $audioObject
if ($LASTEXITCODE -ne 0) { throw "Doom BuzzOS audio compile failed" }
$objects += $audioObject

& ld.lld -m elf_i386 -T build/user/user.ld -nostdlib -o $Output `
    build/user/crt0.o build/user/libc.o build/user/guiapp.o @objects
if ($LASTEXITCODE -ne 0) { throw "Doom link failed" }
& llvm-objcopy --strip-sections $Output
if ($LASTEXITCODE -ne 0) { throw "Doom strip failed" }
Write-Host "Built $Output ($((Get-Item $Output).Length) bytes)"
