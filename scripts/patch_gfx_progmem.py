# Moves the YCbCr->RGB lookup tables of "GFX Library for Arduino" from RAM to flash.
#
# The library declares ~7 KB of tables as plain `static const` arrays. On the ESP8266 those land in
# RAM, and since they hang off a virtual method (writeYCbCrPixels, only used by JPEG/MJPEG decoders)
# the linker cannot drop them. This firmware never decodes JPEG, so marking them PROGMEM is safe and
# frees the heap the Claude usage client needs for TLS. Calling writeYCbCrPixels would crash afterwards.
import os
import re

Import("env")  # noqa: F821

header = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),  # noqa: F821
    env.subst("$PIOENV"),  # noqa: F821
    "GFX Library for Arduino",
    "src",
    "YCbCr2RGB.h",
)

if not os.path.isfile(header):
    print("[patch_gfx_progmem] YCbCr2RGB.h not found, tables stay in RAM (run the build again)")
else:
    with open(header, "r", encoding="utf-8") as f:
        source = f.read()
    patched = re.sub(r"static const (u?int16_t) (\w+)\[\] =", r"static const \1 \2[] PROGMEM =", source)
    if patched != source:
        with open(header, "w", encoding="utf-8") as f:
            f.write(patched)
        print("[patch_gfx_progmem] YCbCr tables moved to PROGMEM")
