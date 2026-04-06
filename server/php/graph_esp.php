<?php
/**
 * PitPirate – ESP32 probe temperature graph endpoint
 *
 * Returns w bytes of normalised bar heights for the temperature history of one probe.
 * The ESP32 renders these as a bottom-anchored bar chart overlaid behind the probe cell.
 *
 * Each byte encodes one column (left = oldest, right = most recent):
 *   0x00–0xFE  bar height from the bottom of the cell in pixels (0 = empty, h = full)
 *   0xFF       no data (column is outside the current session's time window)
 *
 * GET params
 * ----------
 * probe    required  Probe id 1–7
 * w        optional  Image width  in pixels (default 80, max 320)
 * h        optional  Image height in pixels (default 44, max 200)
 * minutes  optional  Look-back window in minutes (default 60, max 1440)
 *
 * Authentication
 * --------------
 * If SECRET_TOKEN is non-empty in config.php, the token must be supplied either
 * as the "X-PitPirate-Token" request header or as the "token" GET parameter.
 *
 * Response
 * --------
 * Content-Type: application/octet-stream
 * Body: w bytes — one per column; 0x00–0xFE = bar height from bottom, 0xFF = no data
 */

define('DATA_FILE',   __DIR__ . '/pitpirate_log.ndjson');
define('MAX_ENTRIES', 8640);  // 24 h at 10-s intervals, matches telemetry.php

require_once __DIR__ . '/config.php';

// ── Disable compression ──────────────────────────────────────────────────────
// This endpoint returns raw binary pixels. Gzip would corrupt the data and
// strip the Content-Length header that the ESP32 relies on.
ini_set('zlib.output_compression', '0');
if (function_exists('apache_setenv')) {
    apache_setenv('no-gzip', '1');
}
header('Content-Encoding: identity');

// ── CORS / method ─────────────────────────────────────────────────────────────
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, OPTIONS');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
    http_response_code(405);
    header('Content-Type: text/plain');
    exit('Method not allowed');
}

// ── Authentication ────────────────────────────────────────────────────────────
if (SECRET_TOKEN !== '') {
    $provided = $_SERVER['HTTP_X_PITPIRATE_TOKEN']
             ?? $_GET['token']
             ?? '';
    if (!hash_equals(SECRET_TOKEN, $provided)) {
        http_response_code(403);
        header('Content-Type: text/plain');
        exit('Forbidden');
    }
}

// ── Input validation ──────────────────────────────────────────────────────────
$probe = trim((string)($_GET['probe'] ?? ''));
if (!preg_match('/^[1-7]$/', $probe)) {
    http_response_code(400);
    header('Content-Type: text/plain');
    exit('Invalid probe: must be 1–7');
}

$w       = max(16, min(320, (int)($_GET['w']       ?? 80)));
$h       = max(16, min(200, (int)($_GET['h']       ?? 44)));
$minutes = max(1,  min(1440,(int)($_GET['minutes'] ?? 60)));

// ── Helper: output an all-no-data response (w bytes of 0xFF) ────────────────
function emitTransparent(int $w): void {
    header('Content-Type: application/octet-stream');
    header('Content-Length: ' . $w);
    header('Cache-Control: no-cache, must-revalidate');
    echo str_repeat("\xff", $w);
}

// ── Load data ─────────────────────────────────────────────────────────────────
if (!file_exists(DATA_FILE)) {
    emitTransparent($w, $h);
    exit;
}

$cutoff = time() - $minutes * 60;
$lines  = file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$series = [];

foreach ($lines as $line) {
    $entry = json_decode($line, true);
    if (!is_array($entry)) continue;
    $ts = (int)($entry['ts'] ?? 0);
    if ($ts <= 0 || $ts < $cutoff) continue;
    $temp = $entry['probes'][$probe] ?? null;
    if ($temp === null) continue;
    $series[] = ['ts' => (int)$ts, 'temp' => (float)$temp];
}

if (count($series) < 2) {
    emitTransparent($w);
    exit;
}

// ── Most-recent continuous segment ───────────────────────────────────────────
// Walk backward from the newest point; stop at the first gap > $maxGapSecs.
// This discards old sessions from the log so data always runs in from the right.
// The fixed window anchor (cutoff → now) is kept regardless.
$maxGapSecs = 300;  // 5 min
$pts        = count($series);
$segStart   = $pts - 1;
while ($segStart > 0 &&
       ($series[$segStart]['ts'] - $series[$segStart - 1]['ts']) <= $maxGapSecs) {
    $segStart--;
}

// Work only with the current-session slice from here on.
$pts    = $pts - $segStart;            // length of current segment
$series = array_slice($series, $segStart);

if ($pts < 2) {
    emitTransparent($w);
    exit;
}

// ── Temperature range (from current segment only) ────────────────────────────
$temps   = array_column($series, 'temp');
$tMin    = (float)min($temps);
$tMax    = (float)max($temps);
$range   = $tMax - $tMin;

// Enforce a minimum span of 10° so a flat session doesn't stretch to full height
if ($range < 10.0) {
    $mid  = ($tMin + $tMax) / 2.0;
    $tMin = $mid - 5.0;
    $tMax = $mid + 5.0;
    $range = 10.0;
}

// 10 % padding top and bottom so the line never hugs the very edge
$pad   = $range * 0.1;
$tMin -= $pad;
$tMax += $pad;
$range = $tMax - $tMin;

// ── Precompute graph line Y (float) for each output column ───────────────────
// The time axis is fixed: left edge = cutoff (= now - minutes), right edge = now.
// Only pixels within the current session's time range get plotted; everything
// else is transparent — so a 10-min session fills just the right sixth of the image.
$tsWindowStart = (float)$cutoff;
$tsWindowEnd   = (float)time();
$tsRange       = max(1.0, $tsWindowEnd - $tsWindowStart);
$tsFirst       = (float)$series[0]['ts'];         // start of current run
$tsLast        = (float)$series[$pts - 1]['ts'];  // most recent point

$graphYf = []; // float graphY[x], or NAN = transparent

for ($px = 0; $px < $w; $px++) {
    $tsFrac = ($w > 1) ? ($px / ($w - 1)) : 0.0;
    $ts     = $tsWindowStart + $tsFrac * $tsRange;

    // Outside the current session → transparent
    if ($ts < $tsFirst || $ts > $tsLast) {
        $graphYf[$px] = NAN;
        continue;
    }

    // Binary-search within the segment
    $lo = 0;
    $hi = $pts - 1;
    while ($hi - $lo > 1) {
        $mid = ($lo + $hi) >> 1;
        if ($series[$mid]['ts'] <= $ts) {
            $lo = $mid;
        } else {
            $hi = $mid;
        }
    }

    $t0 = (float)$series[$lo]['ts'];
    $t1 = (float)$series[$hi]['ts'];

    // Internal gap (shouldn't happen inside the segment, but guard anyway)
    if ($t1 - $t0 > $maxGapSecs) {
        $graphYf[$px] = NAN;
        continue;
    }

    $v0   = $series[$lo]['temp'];
    $v1   = $series[$hi]['temp'];
    $frac = ($t1 > $t0) ? (($ts - $t0) / ($t1 - $t0)) : 0.0;
    $temp = $v0 + $frac * ($v1 - $v0);

    // Map temperature to pixel row: high temp → low y (near top)
    $normTemp     = ($temp - $tMin) / $range;
    $graphYf[$px] = (float)($h - 1) * (1.0 - $normTemp);
}

// ── Encode as bar heights (one byte per column) ──────────────────────────────
// graphYf[px] is the float pixel row from top (0 = hottest, h-1 = coldest).
// Bar height from bottom = h - graphYf, clamped to [0, 254] so 0xFF stays sentinel.
$raw = '';
for ($px = 0; $px < $w; $px++) {
    $gy = $graphYf[$px];
    if (is_nan($gy)) {
        $raw .= "\xff";  // no data sentinel
    } else {
        $raw .= chr(max(0, min(254, (int)round($h - $gy))));
    }
}

// ── Save per-probe BMP (reconstruct 2-D image from bar heights) ──────────────
$chartDir = __DIR__ . '/probe_charts_esp';
if (!is_dir($chartDir)) {
    mkdir($chartDir, 0755, true);
}
$bmpPath = $chartDir . '/probe_' . $probe . '.bmp';

$rowPad   = (4 - ($w % 4)) % 4;
$imgSize  = ($w + $rowPad) * $h;
$fileSize = 14 + 40 + 256 * 4 + $imgSize;

$bmp  = 'BM';
$bmp .= pack('V', $fileSize);
$bmp .= pack('v', 0) . pack('v', 0);
$bmp .= pack('V', 14 + 40 + 256 * 4);
$bmp .= pack('V', 40);
$bmp .= pack('V', $w);
$bmp .= pack('V', (-$h) & 0xFFFFFFFF);   // negative = top-down row order
$bmp .= pack('v', 1) . pack('v', 8) . pack('V', 0);
$bmp .= pack('V', $imgSize);
$bmp .= pack('V', 0) . pack('V', 0);
$bmp .= pack('V', 256) . pack('V', 0);
for ($i = 0; $i < 256; $i++) {
    $bmp .= chr($i) . chr($i) . chr($i) . "\x00";
}
$pad = str_repeat("\x00", $rowPad);
for ($py = 0; $py < $h; $py++) {
    for ($px = 0; $px < $w; $px++) {
        $barH = ord($raw[$px]);
        if ($barH === 0xFF)          { $pixel = 255; }          // no data → white
        elseif ($py >= $h - $barH)  { $pixel = ($py === $h - $barH) ? 30 : 120; } // line / fill
        else                         { $pixel = 255; }          // above bar → white
        $bmp .= chr($pixel);
    }
    $bmp .= $pad;
}
file_put_contents($bmpPath, $bmp);

// ── Output ────────────────────────────────────────────────────────────────────
header('Content-Type: application/octet-stream');
header('Content-Length: ' . $w);
header('Cache-Control: no-cache, must-revalidate');
header('X-Graph-TMin: ' . round($tMin, 2));
header('X-Graph-TMax: ' . round($tMax, 2));
echo $raw;
