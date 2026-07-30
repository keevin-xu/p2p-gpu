# Hardening and warning flags applied to every p2pgpu target.
#
# See docs/CONVENTIONS.md §2 and §3, and rule R11 in CLAUDE.md.
#
# -Werror=switch is load-bearing, not cosmetic: the task lifecycle state
# machine relies on the compiler failing the build when a state is added and
# not handled everywhere. That is why docs/ARCHITECTURE.md §5 forbids a
# `default:` arm on state switches — a default silently defeats this check.

function(p2pgpu_harden target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive-
            /we4062          # unhandled enum value in switch — MSVC's -Werror=switch
            /GS /sdl
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Werror=switch
            -Werror=return-type
            -Wshadow
            -Wconversion -Wsign-conversion   # catches silent narrowing on length fields
            -Wcast-qual
            -Wnon-virtual-dtor
        )
        # Release-only hardening. Debug builds run under sanitizers instead,
        # and _FORTIFY_SOURCE requires optimization to do anything.
        target_compile_options(${target} PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
            $<$<NOT:$<CONFIG:Debug>>:-fstack-protector-strong>
        )
        # Standard-library hardening. CONVENTIONS.md §2 names only
        # _GLIBCXX_ASSERTIONS, which is libstdc++-specific — on the PRIMARY
        # toolchain here (Clang + libc++ on macOS) that define does nothing, so
        # the promised hardening was silently absent. libc++'s equivalent is
        # _LIBCPP_HARDENING_MODE; "fast" is the ABI-safe level, so it can differ
        # from how the vcpkg dependencies were built without consequence.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(${target} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:_GLIBCXX_ASSERTIONS>)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_definitions(${target} PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
        endif()
    endif()
endfunction()
