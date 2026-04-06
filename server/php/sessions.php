<?php
/**
 * PitPirate – session management
 *
 * GET  /sessions.php?action=list
 *   Returns a JSON array of archived session filenames (newest first).
 *
 * POST /sessions.php?action=new-session
 *   Archives the current pitpirate_log.ndjson into sessions/ using a
 *   human-readable date-range filename derived from the first and last
 *   timestamp in the log, then clears the active log.
 *   Returns: { ok: true, name: "<archive-filename>" }
 */

define('DATA_FILE',    __DIR__ . '/pitpirate_log.ndjson');
define('SESSIONS_DIR', __DIR__ . '/sessions');

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$action = $_GET['action'] ?? '';

// ── GET ?action=list ──────────────────────────────────────────────────────────

if ($method === 'GET' && $action === 'list') {
    if (!is_dir(SESSIONS_DIR)) {
        exit(json_encode([]));
    }
    $files = glob(SESSIONS_DIR . '/*.ndjson');
    if ($files === false) {
        exit(json_encode([]));
    }
    // Return basenames, sorted newest first (filename starts with date)
    $names = array_map('basename', $files);
    rsort($names);
    exit(json_encode($names));
}

// ── POST ?action=new-session ──────────────────────────────────────────────────

if ($method === 'POST' && $action === 'new-session') {
    if (!file_exists(DATA_FILE)) {
        http_response_code(409);
        exit(json_encode(['error' => 'No active log found']));
    }

    $lines = @file(DATA_FILE, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
    if ($lines === false || count($lines) === 0) {
        http_response_code(409);
        exit(json_encode(['error' => 'Log is empty — nothing to archive']));
    }

    // Find first and last valid timestamp
    $tsFirst = null;
    foreach ($lines as $line) {
        $e = json_decode($line, true);
        if (is_array($e) && !empty($e['ts'])) { $tsFirst = (int) $e['ts']; break; }
    }
    $tsLast = null;
    foreach (array_reverse($lines) as $line) {
        $e = json_decode($line, true);
        if (is_array($e) && !empty($e['ts'])) { $tsLast = (int) $e['ts']; break; }
    }

    if ($tsFirst === null || $tsLast === null) {
        http_response_code(409);
        exit(json_encode(['error' => 'Log contains no timestamps — cannot name archive']));
    }

    $fmt  = fn(int $ts): string => date('d-m-Y_H-i', $ts);
    $name = $fmt($tsFirst) . '_' . $fmt($tsLast) . '.ndjson';

    // Ensure sessions directory exists
    if (!is_dir(SESSIONS_DIR)) {
        if (!mkdir(SESSIONS_DIR, 0750, true)) {
            http_response_code(500);
            exit(json_encode(['error' => 'Could not create sessions directory']));
        }
    }

    // Avoid collisions if an archive with this name already exists
    $dest = SESSIONS_DIR . '/' . $name;
    if (file_exists($dest)) {
        $i = 2;
        do {
            $dest = SESSIONS_DIR . '/' . pathinfo($name, PATHINFO_FILENAME) . "_$i.ndjson";
            $i++;
        } while (file_exists($dest));
        $name = basename($dest);
    }

    if (!rename(DATA_FILE, $dest)) {
        http_response_code(500);
        exit(json_encode(['error' => 'Failed to archive log file']));
    }

    // Create a fresh empty log so the next write doesn't fail
    file_put_contents(DATA_FILE, '');

    exit(json_encode(['ok' => true, 'name' => $name]));
}

// ── Unknown ───────────────────────────────────────────────────────────────────
http_response_code(400);
echo json_encode(['error' => 'Unknown action. Use: list | new-session']);
