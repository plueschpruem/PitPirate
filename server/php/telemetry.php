<?php
/**
 * PitPirate – remote telemetry receiver
 *
 * POST  /telemetry.php          – store one reading (JSON body required)
 * GET   /telemetry.php          – return recent readings as a JSON array
 *        ?limit=N               – number of most-recent entries to return
 *                                 (default 360 = 1 h at 10-s intervals)
 *
 * ── Configuration ────────────────────────────────────────────────────────────
 *
 * SECRET_TOKEN   Set to a non-empty string to require the
 *                "X-PitPirate-Token: <token>" request header.
 *                Leave empty to accept unauthenticated requests (dev/LAN only).
 *                Must match REMOTE_POST_TOKEN in your ESP32 config.h.
 *                Defined in config.php.
 *
 * DATA_FILE      Absolute path to the NDJSON log file (one JSON object per
 *                line). The web-server user must have write access.
 *
 * MAX_ENTRIES    Maximum number of readings stored on disk.
 *                5 760 ≈ 16 hours at 10-second posting intervals.
 */

define('DATA_FILE',    __DIR__ . '/pitpirate_log.ndjson');
require_once __DIR__ . '/config.php';
define('ALARM_FILE',   __DIR__ . '/pitpirate_alarms.json');
define('MAX_ENTRIES',  8640);  // 24 h at 10-s intervals

// ── Headers ──────────────────────────────────────────────────────────────────
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');

// ── Authentication ───────────────────────────────────────────────────────────
$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$action = $_GET['action'] ?? '';

// Alarm config writes are unauthenticated (browser PWA must be able to save).
$isAlarmWrite = ($method === 'POST' && $action === 'alarms');

if (!$isAlarmWrite && SECRET_TOKEN !== '') {
    $provided = $_SERVER['HTTP_X_PITPIRATE_TOKEN'] ?? '';
    if (!hash_equals(SECRET_TOKEN, $provided)) {
        http_response_code(403);
        exit(json_encode(['error' => 'Forbidden']));
    }
}

// ── POST: store a reading ────────────────────────────────────────────────────
if ($method === 'POST') {

    // ── POST ?action=alarms — save probe alarm limits ────────────────────────
    if ($action === 'alarms') {
        $body = (string) file_get_contents('php://input');
        if ($body === '') { http_response_code(400); exit(json_encode(['error' => 'Empty body'])); }
        $data = json_decode($body, true);
        if (!is_array($data)) { http_response_code(400); exit(json_encode(['error' => 'Invalid JSON'])); }

        // Validate structure: { "1": { "lo": 0, "hi": 0 }, … }
        foreach ($data as $id => $limits) {
            if (!preg_match('/^[1-7]$/', (string) $id) ||
                !is_array($limits) ||
                !array_key_exists('lo', $limits) ||
                !array_key_exists('hi', $limits)) {
                http_response_code(400);
                exit(json_encode(['error' => "Invalid alarm entry for probe '$id'"]));
            }
        }

        // Merge with existing so a single-probe update doesn't wipe everything.
        // array_replace preserves integer keys (PHP converts "1"–"7" to ints);
        // array_merge would reindex them and append instead of overwrite.
        $existing = [];
        if (file_exists(ALARM_FILE)) {
            $existing = json_decode(file_get_contents(ALARM_FILE), true) ?? [];
        }
        $merged = array_replace($existing, $data);
        file_put_contents(ALARM_FILE, json_encode($merged), LOCK_EX);
        exit(json_encode(['ok' => true]));
    }

    // ── POST (default) — store a telemetry reading ───────────────────────────
    $body = (string) file_get_contents('php://input');
    if ($body === '') {
        http_response_code(400);
        exit(json_encode(['error' => 'Empty body']));
    }

    $data = json_decode($body, true);
    if (!is_array($data)) {
        http_response_code(400);
        exit(json_encode(['error' => 'Invalid JSON']));
    }

    // Append one NDJSON line.
    $line = json_encode($data, JSON_UNESCAPED_UNICODE) . "\n";
    file_put_contents(DATA_FILE, $line, FILE_APPEND | LOCK_EX);

    // Trim the log once it grows meaningfully past the retention limit.
    // Reading the whole file every 10 s is cheap for reasonable MAX_ENTRIES.
    $lines = @file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    if ($lines !== false && count($lines) > MAX_ENTRIES + 120) {
        $lines = array_slice($lines, -MAX_ENTRIES);
        file_put_contents(DATA_FILE, implode("\n", $lines) . "\n", LOCK_EX);
    }

    exit(json_encode(['ok' => true, 'ts' => $data['ts'] ?? null]));
}

// ── GET: return recent readings ──────────────────────────────────────────────
if ($method === 'GET') {
    $action = $_GET['action'] ?? '';

    // ── GET ?action=alarms — return probe alarm limits ───────────────────────
    if ($action === 'alarms') {
        if (!file_exists(ALARM_FILE)) {
            exit(json_encode((object)[]));
        }
        exit(file_get_contents(ALARM_FILE));
    }

    $limit = isset($_GET['limit'])
           ? max(1, min((int) $_GET['limit'], MAX_ENTRIES))
           : 360;

    if (!file_exists(DATA_FILE)) {
        exit(json_encode([]));
    }

    $lines   = file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    $lines   = array_slice($lines, -$limit);
    $entries = array_values(array_filter(array_map('json_decode', $lines)));
    exit(json_encode($entries));
}

// ── Other methods ────────────────────────────────────────────────────────────
http_response_code(405);
echo json_encode(['error' => 'Method not allowed']);
