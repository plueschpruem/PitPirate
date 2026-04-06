#pragma once

// Initialise the HTTP server and register routes (call once in setup)
void webServerInit();

// Drive the HTTP server (call every loop iteration)
void webServerLoop();
