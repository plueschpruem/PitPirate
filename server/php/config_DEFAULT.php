<?php

// RENAME THIS FILE TO config.php

/**
 * PitPirate server — shared configuration
 *
 * Used by:  alarm.php              – ESP32 alarm push endpoint
 *           push-subscriptions.php – browser subscribe/unsubscribe endpoint
 *           telemetry.php          – telemetry receiver / alarm config
 *
 * ── VAPID keys ────────────────────────────────────────────────────────────────
 * Generate a fresh key pair once:
 *   php server/php/generate-keys.php
 * The PUBLIC key must also be in your frontend service-worker / subscribe call.
 */
define('VAPID_PUBLIC_KEY',  'generate me!');
define('VAPID_PRIVATE_KEY', 'generate me too!');

// Your contact address (shown to push services in the JWT subject claim)
define('VAPID_SUBJECT', 'mailto:me@myfakemail.gov');

// ── Subscriptions storage ─────────────────────────────────────────────────────
// The web-server user needs write access to this file (and its directory).
// Create the directory manually if it does not exist:
//   mkdir -p /path/to/server/data_notifications && chmod 750 /path/to/server/data_notifications
define('SUBSCRIPTIONS_FILE', __DIR__ . '/data_notifications/subscriptions.json');

// ── Alarm shared secret ───────────────────────────────────────────────────────
// Must match ALARM_SECRET in include/config.h on the ESP32.
define('ALARM_SECRET', ''); // e.g. tZgVso_7291_xKqB_4483 | Same as in .include/config.h

// ── Telemetry shared secret ──────────────────────────────────────────
// Must match REMOTE_POST_TOKEN in include/config.h on the ESP32.
// Leave empty to accept unauthenticated requests (dev/LAN only).
define('SECRET_TOKEN', '');  // e.g. AAA_BBB_CCC_222 | Same as in .include/config.h