<?php
/**
 * PitPirate – blower/fan settings relay endpoint
 *
 * When the frontend is served from the HTTPS remote server the ESP32 is not
 * directly reachable, so fan changes go through here instead.
 *
 * GET  /blower.php   – return current stored fan config { pct, start, min }
 * POST /blower.php   – save fan config  (JSON body: { pct?, start?, min? })
 *
 * The ESP32 polls GET /blower.php via remotePostLoop (same base URL as
 * telemetry.php) and applies any changes.
 *
 * POST writes are unauthenticated so the browser PWA can call them directly,
 * consistent with the alarm-limits write in telemetry.php.
 * All values are validated and clamped before being stored.
 */

define('BLOWER_FILE', __DIR__ . '/pitpirate_blower.json');

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function load_blower(): array
{
    if (file_exists(BLOWER_FILE)) {
        $data = json_decode(file_get_contents(BLOWER_FILE), true);
        if (is_array($data)) return $data;
    }
    // Defaults mirror fan_control.cpp NVS defaults
    return ['pct' => 0, 'start' => 40, 'min' => 25];
}

function clamp_pct(mixed $v): int
{
    return max(0, min(100, (int) $v));
}

// ---------------------------------------------------------------------------
// GET – return stored config
// ---------------------------------------------------------------------------
if ($method === 'GET') {
    echo json_encode(load_blower());
    exit;
}

// ---------------------------------------------------------------------------
// POST – update stored config
// ---------------------------------------------------------------------------
if ($method === 'POST') {
    $body = (string) file_get_contents('php://input');
    if ($body === '') {
        http_response_code(400);
        exit(json_encode(['error' => 'Empty body']));
    }

    $input = json_decode($body, true);
    if (!is_array($input)) {
        http_response_code(400);
        exit(json_encode(['error' => 'Invalid JSON']));
    }

    // Merge with existing so a partial update (e.g. pct-only) is safe.
    $cfg = load_blower();

    if (array_key_exists('pct',   $input)) $cfg['pct']   = clamp_pct($input['pct']);
    if (array_key_exists('start', $input)) $cfg['start'] = clamp_pct($input['start']);
    if (array_key_exists('min',   $input)) $cfg['min']   = clamp_pct($input['min']);

    // Enforce the ESP32 constraint: min must not exceed start
    if ($cfg['min'] > $cfg['start']) {
        http_response_code(400);
        exit(json_encode(['error' => 'min must be <= start']));
    }

    file_put_contents(BLOWER_FILE, json_encode($cfg, JSON_PRETTY_PRINT), LOCK_EX);
    exit(json_encode(['ok' => true, 'config' => $cfg]));
}

// ---------------------------------------------------------------------------
http_response_code(405);
echo json_encode(['error' => 'Method not allowed']);
