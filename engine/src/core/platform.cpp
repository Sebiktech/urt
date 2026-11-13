#include "urt/core/platform.hpp"

#include <atomic>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#if defined(__linux__)
#include <pthread.h>
#endif
#endif

namespace urt::platform {
namespace {

// A signal handler may only touch lock-free atomics, so make sure this is one.
std::atomic<bool> g_shutdown{false};
static_assert(std::atomic<bool>::is_always_lock_free);

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_shutdown.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}
#else
void signal_handler(int) { g_shutdown.store(true); }
#endif

} // namespace

void install_shutdown_handler() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
}

bool shutdown_requested() { return g_shutdown.load(); }

void request_shutdown() { g_shutdown.store(true); }

void reset_shutdown_request() { g_shutdown.store(false); }

unsigned hardware_threads() {
    const unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : count;
}

void set_current_thread_name(const char* name) {
#if defined(__linux__)
    // Linux limits thread names to 15 characters plus the terminator.
    char buffer[16] = {};
    for (int i = 0; i < 15 && name[i] != '\0'; ++i) {
        buffer[i] = name[i];
    }
    pthread_setname_np(pthread_self(), buffer);
#else
    (void)name;
#endif
}

} // namespace urt::platform
