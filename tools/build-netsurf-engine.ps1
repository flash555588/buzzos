param(
    [string]$Workspace = "third_party/netsurf-workspace",
    [string]$NetSurf = "third_party/netsurf-reference",
    [string]$BuildDir = "build/netsurf-engine",
    [string]$Output = "build/user/nsmonkey.elf",
    [string]$GuiOutput = "build/user/netsurf.elf",
    [switch]$Link
)

$ErrorActionPreference = "Stop"

if (!(Test-Path -LiteralPath $NetSurf)) {
    throw "Missing NetSurf checkout. Run tools/fetch-netsurf.ps1 first."
}

$pu = Join-Path $Workspace "libparserutils"
$wc = Join-Path $Workspace "libwapcaplet"
$hb = Join-Path $Workspace "libhubbub"
$dom = Join-Path $Workspace "libdom"
$css = Join-Path $Workspace "libcss"
$nsutils = Join-Path $Workspace "libnsutils"
$zlib = Join-Path $Workspace "zlib"
New-Item -ItemType Directory -Force $BuildDir | Out-Null
& python tools/embed-netsurf-resources.py --netsurf $NetSurf --output (Join-Path $BuildDir "buzzos_netsurf_resources.h")
if ($LASTEXITCODE -ne 0) { throw "Could not embed NetSurf resources" }
$domBindingInclude = Join-Path $BuildDir "dom/bindings/hubbub"
New-Item -ItemType Directory -Force $domBindingInclude | Out-Null
Copy-Item -Force (Join-Path $dom "bindings/hubbub/*.h") $domBindingInclude
$testament = @(
    '#define WT_NO_GIT 1',
    '#define WT_BRANCHPATH "buzzos-port"',
    '#define WT_REVID "a471a0d44274"',
    '#define WT_COMPILEDATE "reproducible"',
    '#define WT_HOSTNAME "BuzzOS"',
    '#define WT_ROOT "/"',
    '#define USERNAME "buzzos"',
    '#define GECOS "BuzzOS NetSurf port"',
    '#define WT_MODIFIED 0',
    '#define WT_MODIFICATIONS { { 0, 0 } }'
)
Set-Content -Encoding Ascii (Join-Path $BuildDir "testament.h") $testament

$common = @(
    "--target=x86_64-none-elf", "-std=c11", "-ffreestanding", "-fno-builtin",
    "-fno-stack-protector", "-fno-pic", "-mcmodel=large", "-mno-red-zone",
    "-mno-stack-arg-probe", "-O1",
    "-DNDEBUG", "-D__serenity__", "-Dmonkey", "-Dnsmonkey",
    "-DWITHOUT_ICONV_FILTER", "-D_ALIGNED=__attribute__((aligned))",
    "-DSTMTEXPR=1", "-include", "src/user/ports/netsurf/buzzos_build_config.h",
    "-Isrc/user/libc", "-Isrc/user/ports/netsurf", "-Isrc/user/third_party/lodepng",
    "-Isrc/user/third_party/bearssl/inc", "-I$NetSurf", "-I$NetSurf/include", "-I$NetSurf/frontends",
    "-I$NetSurf/content/handlers", "-I$BuildDir",
    "-I$pu/include", "-I$wc/include", "-I$hb/include",
    "-I$dom/include", "-I$dom/bindings/hubbub", "-I$css/include",
    "-I$nsutils/include", "-I$zlib"
)

$sources = @(
    "content/content.c", "content/content_factory.c", "content/fetch.c",
    "content/hlcache.c", "content/llcache.c", "content/mimesniff.c",
    "content/textsearch.c", "content/urldb.c", "content/no_backing_store.c",
    "content/fs_backing_store.c",
    "content/fetchers/data.c", "content/fetchers/resource.c",
    "content/fetchers/file/dirlist.c", "content/fetchers/file/file.c",
    "content/handlers/javascript/fetcher.c",
    "content/handlers/javascript/none/none.c"
)
$sources += @(
    "about.c", "blank.c", "certificate.c", "chart.c", "choices.c",
    "config.c", "imagecache.c", "nscolours.c", "query.c", "query_auth.c",
    "query_fetcherror.c", "query_privacy.c", "query_timeout.c", "testament.c",
    "websearch.c"
) | ForEach-Object { "content/fetchers/about/$_" }
$sources += @("css.c", "dump.c", "internal.c", "hints.c", "select.c") |
    ForEach-Object { "content/handlers/css/$_" }
$sources += @("image.c", "image_cache.c") |
    ForEach-Object { "content/handlers/image/$_" }
$sources += @(
    "box_construct.c", "box_inspect.c", "box_manipulate.c", "box_normalise.c",
    "box_special.c", "box_textarea.c", "css.c", "css_fetcher.c", "dom_event.c",
    "font.c", "form.c", "forms.c", "html.c", "imagemap.c", "interaction.c",
    "layout.c", "layout_flex.c", "object.c", "redraw.c", "redraw_border.c",
    "script.c", "table.c", "textselection.c"
) | ForEach-Object { "content/handlers/html/$_" }
$sources += "content/handlers/text/textplain.c"
$sources += @(
    "bloom.c", "corestrings.c", "file.c", "filepath.c", "hashmap.c",
    "hashtable.c", "idna.c", "libdom.c", "log.c", "messages.c", "nscolour.c",
    "nsoption.c", "punycode.c", "ssl_certs.c", "talloc.c", "time.c", "url.c",
    "useragent.c", "utf8.c", "utils.c"
) | ForEach-Object { "utils/$_" }
$sources += @(
    "challenge.c", "generics.c", "primitives.c", "parameter.c", "cache-control.c",
    "content-disposition.c", "content-type.c", "strict-transport-security.c",
    "www-authenticate.c"
) | ForEach-Object { "utils/http/$_" }
$sources += @("utils/nsurl/nsurl.c", "utils/nsurl/parse.c")
$sources += @(
    "cookie_manager.c", "knockout.c", "hotlist.c", "mouse.c", "plot_style.c",
    "print.c", "search.c", "searchweb.c", "scrollbar.c", "textarea.c",
    "version.c", "system_colour.c", "local_history.c", "global_history.c",
    "treeview.c", "page-info.c", "bitmap.c", "browser.c", "browser_window.c",
    "browser_history.c", "download.c", "frames.c", "netsurf.c", "cw_helper.c",
    "save_complete.c", "save_text.c", "selection.c", "textinput.c",
    "gui_factory.c", "save_pdf.c", "font_haru.c"
) | ForEach-Object { "desktop/$_" }
$frontendSources = @(
    "main.c", "output.c", "filetype.c", "schedule.c", "bitmap.c", "plot.c",
    "browser.c", "download.c", "401login.c", "layout.c", "dispatch.c", "fetch.c"
) | ForEach-Object { "frontends/monkey/$_" }
$sources += $frontendSources

$objects = [Collections.Generic.List[string]]::new()
foreach ($source in $sources) {
    $input = Join-Path $NetSurf $source
    $object = Join-Path $BuildDir ($source.Replace("/", "_").Replace(".c", ".o"))
    & clang @common -c $input -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
    $objects.Add($object)
}

$zlibSources = @(
    "adler32.c", "compress.c", "crc32.c", "deflate.c", "gzclose.c", "gzlib.c",
    "gzread.c", "gzwrite.c", "infback.c", "inffast.c", "inflate.c",
    "inftrees.c", "trees.c", "uncompr.c", "zutil.c"
)
foreach ($source in $zlibSources) {
    $input = Join-Path $zlib $source
    $object = Join-Path $BuildDir ("zlib_" + $source.Replace(".c", ".o"))
    & clang @common -c $input -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: zlib/$source" }
    $objects.Add($object)
}
$guiObjects = [Collections.Generic.List[string]]::new()
foreach ($source in @("src/user/ports/netsurf/buzzos_gui.c",
                      "src/user/ports/netsurf/buzzos_gui_plot.c",
                      "src/user/ports/netsurf/buzzos_http_fetch.c",
                      "src/user/ports/netsurf/buzzos_tls.c",
                      "src/user/ports/netsurf/buzzos_png.c")) {
    $object = Join-Path $BuildDir ((Split-Path $source -Leaf).Replace(".c", ".o"))
    & clang @common -c $source -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $source" }
    $guiObjects.Add($object)
}
$lodepngObject = Join-Path $BuildDir "lodepng.o"
$lodepngFlags = $common + @(
    "-Isrc/user/third_party/lodepng",
    "-DLODEPNG_NO_COMPILE_DISK",
    "-DLODEPNG_NO_COMPILE_ENCODER",
    "-DLODEPNG_NO_COMPILE_ANCILLARY_CHUNKS",
    "-DLODEPNG_NO_COMPILE_ALLOCATORS"
)
& clang @lodepngFlags -c "src/user/third_party/lodepng/lodepng.c" -o $lodepngObject
if ($LASTEXITCODE -ne 0) { throw "Compile failed: lodepng" }
$guiObjects.Add($lodepngObject)

$bearsslRoot = "src/user/third_party/bearssl"
$bearsslObjects = [Collections.Generic.List[string]]::new()
foreach ($input in Get-ChildItem (Join-Path $bearsslRoot "src") -Recurse -Filter *.c) {
    $relative = $input.FullName.Substring(
        (Resolve-Path (Join-Path $bearsslRoot "src")).Path.Length + 1)
    $objectName = "bearssl_" + $relative.Replace("\", "_").Replace("/", "_").Replace(".c", ".o")
    $object = Join-Path $BuildDir $objectName
    $bearsslFlags = @(
        "--target=x86_64-none-elf", "-std=c11", "-ffreestanding", "-fno-builtin",
        "-fno-stack-protector", "-fno-pic", "-mcmodel=large", "-mno-red-zone",
        "-mno-stack-arg-probe", "-O2",
        "-DNDEBUG", "-Isrc/user/libc", "-I$bearsslRoot/inc", "-I$bearsslRoot/src"
    )
    & clang @bearsslFlags -c $input.FullName -o $object
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: BearSSL/$relative" }
    $bearsslObjects.Add($object)
}
$bearsslArchive = Join-Path $BuildDir "libbearssl.a"
& llvm-ar rcs $bearsslArchive @bearsslObjects
if ($LASTEXITCODE -ne 0) { throw "Could not archive BearSSL" }

Write-Host "Compiled $($objects.Count) NetSurf engine and bootstrap frontend sources"
if ($Link) {
    $dependencyObjects = Get-ChildItem "build/netsurf" -Directory |
        ForEach-Object { Get-ChildItem $_.FullName -Filter *.o } |
        Where-Object { $_.FullName -notlike "*\wapcaplet\libwapcaplet.o" } |
        ForEach-Object { $_.FullName }
    $linkObjects = @("build/user/crt0.o", "build/user/libc.o") +
        @($objects) + @($dependencyObjects)
    function Link-Executable([string]$Target, [string[]]$Inputs, [string]$ResponseName) {
        $responseFile = Join-Path $BuildDir $ResponseName
        $linkArguments = @("-m", "elf_x86_64", "-z", "max-page-size=0x1000",
                           "-T", "build/user/user.ld",
                           "-nostdlib", "-o", $Target) + $Inputs
        $linkArguments | ForEach-Object { '"' + $_ + '"' } |
            Set-Content -Encoding Ascii $responseFile
        & ld.lld "@$responseFile"
        if ($LASTEXITCODE -ne 0) { throw "Could not link $Target" }
        & llvm-objcopy --strip-sections $Target
        if ($LASTEXITCODE -ne 0) { throw "Could not strip $Target" }
    }
    Link-Executable $Output $linkObjects "link-monkey.rsp"
    if ($LASTEXITCODE -ne 0) { throw "Could not link NetSurf bootstrap" }
    Write-Host "Built $Output ($((Get-Item $Output).Length) bytes)"
    $guiEngineObjects = @($objects) | Where-Object {
        $_ -notlike "*frontends_monkey_main.o" -and
        $_ -notlike "*frontends_monkey_fetch.o" -and
        $_ -notlike "*frontends_monkey_dispatch.o" -and
        $_ -notlike "*frontends_monkey_filetype.o" -and
        $_ -notlike "*frontends_monkey_401login.o"
    }
    $guiLinkObjects = @("build/user/crt0.o", "build/user/libc.o",
                       "build/user/guiapp.o") + @($guiEngineObjects) +
                       @($guiObjects) + @($dependencyObjects) + @($bearsslArchive)
    Link-Executable $GuiOutput $guiLinkObjects "link-gui.rsp"
    Write-Host "Built $GuiOutput ($((Get-Item $GuiOutput).Length) bytes)"
}
