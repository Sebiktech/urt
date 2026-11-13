#pragma once

// The few things that differ between operating systems. Everything else in
// the engine goes through this header instead of calling the OS directly.

namespace urt::platform {

// Makes Ctrl+C (and SIGTERM on Linux) set a flag instead of killing the
// process, so the main loop can finish its tick and shut down cleanly.
void install_shutdown_handler();

bool shutdown_requested();
void request_shutdown();

// Clears the flag. Used by tests that run more than one application.
void reset_shutdown_request();

// Number of hardware threads, never less than 1.
unsigned hardware_threads();

// Names the calling thread for debuggers and profilers. Best effort.
void set_current_thread_name(const char* name);

} // namespace urt::platform
