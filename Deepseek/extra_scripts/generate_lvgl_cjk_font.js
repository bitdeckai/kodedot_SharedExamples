const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..');
const sourceFontCommentFile = path.join(
  repoRoot,
  '.pio',
  'libdeps',
  'kode_dot',
  'GFX Library for Arduino',
  'examples',
  'LVGL',
  'LvglHelloNeoPixel',
  'ui_font_Chill7.c'
);
const systemFont = 'C:\\Windows\\Fonts\\NotoSansSC-VF.ttf';
const sizeArg = process.argv[2] || '24';
const size = parseInt(sizeArg, 10);

if (!Number.isFinite(size) || size < 8 || size > 64) {
  throw new Error(`Invalid font size '${sizeArg}'. Use an integer between 8 and 64.`);
}

const fontBaseName = `ui_font_NotoSansSC_${size}_cjk`;
const outputIncFile = path.join(repoRoot, 'src', 'fonts', `${fontBaseName}_data.inc`);
const outputTmpFile = path.join(repoRoot, 'src', 'fonts', `${fontBaseName}_tmp.c`);

function extractSymbols(filePath) {
  const text = fs.readFileSync(filePath, 'utf8');
  const match = text.match(/--symbols\s+([\s\S]*?)\s+--no-prefilter/);
  if (!match) {
    throw new Error(`Unable to extract --symbols block from ${filePath}`);
  }

  const baseSymbols = match[1];

  // Ensure common Chinese/full-width punctuation is always available.
  const extraPunctuation = '，。！？：；、（）《》〈〉【】「」『』〔〕［］｛｝“”‘’—…·～￥　︰︱︳︵︶︹︺︻︼';

  let merged = baseSymbols;
  for (const ch of extraPunctuation) {
    if (!merged.includes(ch)) {
      merged += ch;
    }
  }

  return merged;
}

async function main() {
  if (!fs.existsSync(systemFont)) {
    throw new Error(`System font not found: ${systemFont}`);
  }

  const symbols = extractSymbols(sourceFontCommentFile);
  const cli = require(path.join(repoRoot, 'node_modules', 'lv_font_conv', 'lib', 'cli'));

  await cli.run([
    '--no-compress',
    '--no-prefilter',
    '--bpp', '4',
    '--size', String(size),
    '--font', systemFont,
    '-r', '0x20-0x7f',
    '--symbols', symbols,
    '--format', 'lvgl',
    '--output', outputTmpFile,
    '--lv-font-name', fontBaseName,
    '--force-fast-kern-format'
  ]);

  const raw = fs.readFileSync(outputTmpFile, 'utf8');
  fs.unlinkSync(outputTmpFile);

  // Drop include boilerplate so wrappers control how lvgl.h is included.
  const normalized = raw.replace(/^[\s\S]*?(#ifndef\s+UI_FONT_[A-Z0-9_]+\b)/, '$1');
  fs.writeFileSync(outputIncFile, normalized, 'utf8');

  console.log(`Generated ${outputIncFile}`);
}

main().catch((error) => {
  console.error(error.stack || error.message || String(error));
  process.exit(1);
});