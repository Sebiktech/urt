# Warning and sanitizer flags shared by every URT target.
# Targets opt in with: target_link_libraries(<target> PRIVATE urt_options)

add_library(urt_options INTERFACE)

if(MSVC)
    target_compile_options(urt_options INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(urt_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(urt_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Sanitizers find memory errors (address) and data races (thread) at runtime.
# They are GCC/Clang only and cannot be combined with each other.
if(URT_SANITIZE AND NOT MSVC)
    if(URT_SANITIZE STREQUAL "address")
        set(_urt_san -fsanitize=address,undefined -fno-omit-frame-pointer)
    elseif(URT_SANITIZE STREQUAL "thread")
        set(_urt_san -fsanitize=thread)
    else()
        message(FATAL_ERROR "URT_SANITIZE must be 'address' or 'thread', got '${URT_SANITIZE}'")
    endif()
    target_compile_options(urt_options INTERFACE ${_urt_san})
    target_link_options(urt_options INTERFACE ${_urt_san})
endif()
