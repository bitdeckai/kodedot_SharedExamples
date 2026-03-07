Add-Type -AssemblyName System.Drawing

$src = 'src/images/DEEPSEEK_basic.png'
$dstC = 'src/images/DEEPSEEK_basic.c'

if (-not (Test-Path $src)) {
    throw "Source PNG not found: $src"
}

$bmp = [System.Drawing.Bitmap]::new($src)
$w = $bmp.Width
$h = $bmp.Height

$sb = New-Object System.Text.StringBuilder
$null = $sb.AppendLine('#ifdef __has_include')
$null = $sb.AppendLine('    #if __has_include("lvgl.h")')
$null = $sb.AppendLine('        #ifndef LV_LVGL_H_INCLUDE_SIMPLE')
$null = $sb.AppendLine('            #define LV_LVGL_H_INCLUDE_SIMPLE')
$null = $sb.AppendLine('        #endif')
$null = $sb.AppendLine('    #endif')
$null = $sb.AppendLine('#endif')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('#if defined(LV_LVGL_H_INCLUDE_SIMPLE)')
$null = $sb.AppendLine('    #include "lvgl.h"')
$null = $sb.AppendLine('#else')
$null = $sb.AppendLine('    #include "lvgl/lvgl.h"')
$null = $sb.AppendLine('#endif')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('#ifndef LV_ATTRIBUTE_MEM_ALIGN')
$null = $sb.AppendLine('#define LV_ATTRIBUTE_MEM_ALIGN')
$null = $sb.AppendLine('#endif')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('#ifndef LV_ATTRIBUTE_IMAGE_DEEPSEEK_BASIC')
$null = $sb.AppendLine('#define LV_ATTRIBUTE_IMAGE_DEEPSEEK_BASIC')
$null = $sb.AppendLine('#endif')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMAGE_DEEPSEEK_BASIC uint8_t DEEPSEEK_basic_map[] = {')

$bytesOnLine = 0
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $c = $bmp.GetPixel($x, $y)
        if ($c.A -lt 128) {
            $rgb565 = 0
        }
        else {
            $r = [int]$c.R
            $g = [int]$c.G
            $b = [int]$c.B
            $rgb565 = (($r -band 0xF8) -shl 8) -bor (($g -band 0xFC) -shl 3) -bor ($b -shr 3)
        }

        $lo = $rgb565 -band 0xFF
        $hi = ($rgb565 -shr 8) -band 0xFF

        if ($bytesOnLine -eq 0) {
            $null = $sb.Append('  ')
        }

        $null = $sb.Append(('0x{0:X2}, 0x{1:X2}, ' -f $lo, $hi))
        $bytesOnLine += 2

        if ($bytesOnLine -ge 32) {
            $null = $sb.AppendLine()
            $bytesOnLine = 0
        }
    }
}

if ($bytesOnLine -ne 0) {
    $null = $sb.AppendLine()
}

$null = $sb.AppendLine('};')
$null = $sb.AppendLine('')
$null = $sb.AppendLine('const lv_image_dsc_t DEEPSEEK_basic = {')
$null = $sb.AppendLine('  .header.cf = LV_COLOR_FORMAT_RGB565,')
$null = $sb.AppendLine('  .header.magic = LV_IMAGE_HEADER_MAGIC,')
$null = $sb.AppendLine(('  .header.w = {0},' -f $w))
$null = $sb.AppendLine(('  .header.h = {0},' -f $h))
$null = $sb.AppendLine(('  .data_size = {0} * 2,' -f ($w * $h)))
$null = $sb.AppendLine('  .data = DEEPSEEK_basic_map,')
$null = $sb.AppendLine('};')

[System.IO.File]::WriteAllText($dstC, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$bmp.Dispose()
Write-Output "Generated $dstC from $src ($w x $h)"