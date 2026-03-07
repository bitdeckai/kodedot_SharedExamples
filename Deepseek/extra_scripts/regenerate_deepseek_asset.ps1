Add-Type -AssemblyName System.Drawing

$src = 'src/images/DEEPSEEK_basic_orig.png'
if (-not (Test-Path $src)) {
    $src = 'src/images/DEEPSEEK_basic.png'
}

$dstPng = 'src/images/DEEPSEEK_basic.png'
$dstC = 'src/images/DEEPSEEK_basic.c'
$dstW = 114
$dstH = 83

$bmp = [System.Drawing.Bitmap]::new($src)
$cropX = 0
$cropY = 0
# Fixed left-side source area from orig, matching the previously accepted version.
$cropW = [Math]::Min(228, $bmp.Width)
$cropH = [Math]::Min(165, $bmp.Height)
$cropRect = New-Object System.Drawing.Rectangle($cropX, $cropY, $cropW, $cropH)
$canvas = [System.Drawing.Bitmap]::new($dstW, $dstH, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($canvas)
$g.Clear([System.Drawing.Color]::Transparent)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

$drawW = $dstW
$drawH = $dstH
$dx = 0
$dy = 0
$dstRect = New-Object System.Drawing.Rectangle($dx, $dy, $drawW, $drawH)
$g.DrawImage($bmp, $dstRect, $cropRect, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()

$canvas.Save($dstPng, [System.Drawing.Imaging.ImageFormat]::Png)

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
for ($y = 0; $y -lt $dstH; $y++) {
    for ($x = 0; $x -lt $dstW; $x++) {
        $c = $canvas.GetPixel($x, $y)
        if ($c.A -lt 128) {
            $rgb565 = 0
        }
        else {
            $r = [int]$c.R
            $gg = [int]$c.G
            $b = [int]$c.B
            $rgb565 = (($r -band 0xF8) -shl 8) -bor (($gg -band 0xFC) -shl 3) -bor ($b -shr 3)
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
$null = $sb.AppendLine(('  .header.w = {0},' -f $dstW))
$null = $sb.AppendLine(('  .header.h = {0},' -f $dstH))
$null = $sb.AppendLine(('  .data_size = {0} * 2,' -f ($dstW * $dstH)))
$null = $sb.AppendLine('  .data = DEEPSEEK_basic_map,')
$null = $sb.AppendLine('};')

[System.IO.File]::WriteAllText($dstC, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$bmp.Dispose()
$canvas.Dispose()

Write-Output "Left region resized: src=$src crop=$cropX,$cropY,$cropW,$cropH draw=$dx,$dy,$drawW,$drawH"
