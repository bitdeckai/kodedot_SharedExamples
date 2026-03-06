Import("env")
import os

# Patch NeoPixelBus header to ensure gpio_hal_iomux_func_sel is declared for ESP32-S3 builds.
# This is required because some ESP-IDF versions omit the prototype when compiling as C++.
def patch_neopixel(*args, **kwargs):
    pio_env = env['PIOENV']
    project_dir = env['PROJECT_DIR']
    hdr_path = os.path.join(project_dir, ".pio", "libdeps", pio_env,
                             "NeoPixelBus", "src", "internal", "methods", "NeoEsp32LcdXMethod.h")
    if not os.path.isfile(hdr_path):
        print("[patch_neopixel] header not found, skipping")
        return
    with open(hdr_path, 'r+', encoding='utf-8') as f:
        text = f.read()
        # if we already injected the declaration, do nothing
        if 'gpio_hal_iomux_func_sel' in text and 'forward declaration' in text:
            return
        # insert forward declaration after the gpio_hal include
        new_snippet = ('#include <hal/gpio_hal.h>\n'
                       '\n'
                       '// forward declaration inserted by patch_neopixel.py\n'
                       '#ifdef __cplusplus\n'
                       'extern "C" {\n'
                       '#endif\n'
                       'void gpio_hal_iomux_func_sel(uint32_t reg, uint32_t func);\n'
                       '#ifdef __cplusplus\n'
                       '}\n'
                       '#endif')
        new_text = text.replace('#include <hal/gpio_hal.h>', new_snippet)
        f.seek(0)
        f.write(new_text)
        f.truncate()
        print("[patch_neopixel] applied forward declaration patch")

# Hook into build process early
env.AddPreAction("build", patch_neopixel)
