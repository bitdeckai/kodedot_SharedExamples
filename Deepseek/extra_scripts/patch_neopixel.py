Import("env")
import os

# Patch NeoPixelBus for ESP32-S3 builds using newer Arduino-ESP32 frameworks.
# Recent frameworks expose esp_rom_gpio_pad_select_gpio() rather than
# gpio_hal_iomux_func_sel(), so replace the incompatible call in the vendored header.
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
        old_line = '        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);'
        new_line = '        esp_rom_gpio_pad_select_gpio(pin); // patched by patch_neopixel.py'
        if new_line in text:
            return
        if old_line not in text:
            print("[patch_neopixel] target line not found, skipping")
            return
        new_text = text.replace(old_line, new_line)
        f.seek(0)
        f.write(new_text)
        f.truncate()
        print("[patch_neopixel] replaced gpio_hal_iomux_func_sel with esp_rom_gpio_pad_select_gpio")

# Apply immediately when the script is loaded, and also before builds as a safeguard.
patch_neopixel()
env.AddPreAction("build", patch_neopixel)
