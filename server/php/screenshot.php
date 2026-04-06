<?php
/**
 * PitPirate — screenshot receiver
 *
 * Receives raw pixel data from the ESP32 and saves it as a PNG.
 *
 * Wire format (application/octet-stream):
 *   Bytes 0-3:  magic 'RAW1'
 *   Bytes 4-5:  width  (uint16 LE)
 *   Bytes 6-7:  height (uint16 LE)
 *   Bytes 8..:  raw uint16 pixels, row-major, as returned by tft.readRect()
 *
 * Each uint16 on the wire is byte-swapped by readRect() for pushRect() compat.
 * Re-assembling the two wire bytes as big-endian — (lo<<8)|hi — undoes that
 * swap and recovers standard RGB565:
 *   bits[15:11] = R (5-bit)   bits[10:5] = G (6-bit)   bits[4:0] = B (5-bit)
 *
 * Confirmed empirically via brute-force: variant BE_RGB is correct.
 * Note: SPI_READ_FREQUENCY must be ≤4 MHz for reliable pixel reads.
 */

define('SCREENSHOTS_DIR', __DIR__ . '/screenshots');
require_once __DIR__ . '/config.php';

header('Content-Type: application/json; charset=utf-8');

// ── Method guard ──────────────────────────────────────────────────────────────
if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
    http_response_code(405);
    exit(json_encode(['error' => 'Method not allowed']));
}

// ── Authentication ────────────────────────────────────────────────────────────
if (SECRET_TOKEN !== '') {
    $provided = $_SERVER['HTTP_X_PITPIRATE_TOKEN'] ?? '';
    if (!hash_equals(SECRET_TOKEN, $provided)) {
        http_response_code(403);
        exit(json_encode(['error' => 'Forbidden']));
    }
}

// ── Create storage directory on first use ─────────────────────────────────────
if (!is_dir(SCREENSHOTS_DIR)) {
    if (!mkdir(SCREENSHOTS_DIR, 0750, true)) {
        http_response_code(500);
        exit(json_encode(['error' => 'Cannot create screenshots directory']));
    }
    file_put_contents(SCREENSHOTS_DIR . '/.htaccess', "Require all denied\n");
}

// ── Read raw body ─────────────────────────────────────────────────────────────
$body = file_get_contents('php://input');
if ($body === false || strlen($body) < 9) {
    http_response_code(400);
    exit(json_encode(['error' => 'Empty or truncated body']));
}

// ── Validate magic ────────────────────────────────────────────────────────────
if (substr($body, 0, 4) !== 'RAW1') {
    http_response_code(400);
    exit(json_encode(['error' => 'Invalid magic bytes']));
}

// ── Parse header ──────────────────────────────────────────────────────────────
$w = ord($body[4]) | (ord($body[5]) << 8);
$h = ord($body[6]) | (ord($body[7]) << 8);
$expectedBytes = 8 + $w * $h * 2;

if ($w < 1 || $h < 1 || strlen($body) < $expectedBytes) {
    http_response_code(400);
    exit(json_encode(['error' => "Bad dimensions or truncated pixel data (got " . strlen($body) . ", need $expectedBytes)"]));
}

// ── Convert raw565 → PNG via GD ───────────────────────────────────────────────
$img = imagecreatetruecolor($w, $h);
if (!$img) {
    http_response_code(500);
    exit(json_encode(['error' => 'GD imagecreatetruecolor failed']));
}

$offset = 8;
for ($y = 0; $y < $h; $y++) {
    for ($x = 0; $x < $w; $x++) {
        // readRect() byte-swaps every pixel: stored = (color<<8)|(color>>8).
        // The ESP32 sends the stored uint16 little-endian (lo byte first).
        // Re-assembling as big-endian — (lo<<8)|hi — undoes the swap and
        // recovers the original color565 value for clean RGB extraction.
        $lo = ord($body[$offset]);
        $hi = ord($body[$offset + 1]);
        $offset += 2;
        $px = ($lo << 8) | $hi;

        // Standard RGB565 after BE reassembly:
        //   bits[15:11] = R (5-bit)   bits[10:5] = G (6-bit)   bits[4:0] = B (5-bit)
        $r5 = ($px >> 11) & 0x1F; $r = ($r5 << 3) | ($r5 >> 2);
        $g6 = ($px >>  5) & 0x3F; $g = ($g6 << 2) | ($g6 >> 4);
        $b5 = ($px      ) & 0x1F; $b = ($b5 << 3) | ($b5 >> 2);

        imagesetpixel($img, $x, $y, ($r << 16) | ($g << 8) | $b);
    }
}

// ── Write raw pixel data for debugging ────────────────────────────────────────
$basename = date('Y-m-d_H-i-s');
$rawPath  = SCREENSHOTS_DIR . '/' . $basename . '.raw';
file_put_contents($rawPath, $body);  // full wire payload including the 8-byte header

// ── Write PNG ─────────────────────────────────────────────────────────────────
$filename = $basename . '.png';
$path     = SCREENSHOTS_DIR . '/' . $filename;

$resolvedDir = realpath(SCREENSHOTS_DIR);
if ($resolvedDir === false || strpos(realpath(dirname($path)) ?: '', $resolvedDir) !== 0) {
    unset($img);
    http_response_code(500);
    exit(json_encode(['error' => 'Invalid path']));
}

if (!imagepng($img, $path, 6)) {
    unset($img);
    http_response_code(500);
    exit(json_encode(['error' => 'Failed to write PNG']));
}
unset($img);

http_response_code(201);
exit(json_encode(['ok' => true, 'file' => $filename, 'w' => $w, 'h' => $h]));

