<?php
/**
 * PitPirate – bare-minimum connection / TLS timing test
 *
 * GET  /php/connection-test.php?msg=anything
 * POST /php/connection-test.php   (body echoed back)
 *
 * Writes a timestamped line to connection-test.log next to this file.
 * Safe to leave deployed – no secrets, no side-effects beyond the log.
 */

$t_start = microtime(true);

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$msg    = trim($_GET['msg'] ?? '(no msg)');
$body   = ($method === 'POST') ? (string) file_get_contents('php://input') : '';
$ip     = $_SERVER['REMOTE_ADDR'] ?? '-';

$entry = date('Y-m-d H:i:s') . " | $method | ip=$ip | msg=" . substr($msg, 0, 80)
       . ($body !== '' ? ' | body_len=' . strlen($body) : '') . PHP_EOL;

file_put_contents(__DIR__ . '/connection-test.log', $entry, FILE_APPEND | LOCK_EX);

$t_php = round((microtime(true) - $t_start) * 1000, 2);

echo json_encode([
    'ok'         => true,
    'method'     => $method,
    'msg'        => $msg,
    'server_ms'  => $t_php,   // time PHP spent (excludes TLS handshake)
    'ts'         => time(),
]);
