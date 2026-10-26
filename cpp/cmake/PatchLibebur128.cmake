# Makes the gates of libebur128 compile-time constants, so meters can be created on several threads at once.
#
# Every ebur128_init call stores three constants in file-scope globals:
# the relative gate factor, the -20 dB factor, and the absolute gate energy.
# The loudness functions of other meters read them at the same time.
# The stored values never change, but ThreadSanitizer reports the concurrent writes and reads as a data race.
# After this patch, initializing a meter only writes the histogram tables, which are used in histogram mode only.
#
# Runs as the FetchContent patch step with -DEBUR128_SOURCE=<path to ebur128.c>.
# Each replacement is a single line, so the patch applies to LF and CRLF checkouts alike,
# and a source that is already patched is left as it is.

if(NOT EBUR128_SOURCE)
    message(FATAL_ERROR "EBUR128_SOURCE is not set")
endif()

file(READ "${EBUR128_SOURCE}" source)

if(source MATCHES "static const double relative_gate_factor")
    return()
endif()

# Replaces one line of the source, and fails when the line is missing, so a changed upstream file is noticed.
# The C lines contain semicolons, so they are passed as single arguments instead of in a CMake list.
function(replace_line original replacement)
    string(FIND "${source}" "${original}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "libebur128 patch: line not found in ${EBUR128_SOURCE}: ${original}")
    endif()
    string(REPLACE "${original}" "${replacement}" patched "${source}")
    set(source "${patched}" PARENT_SCOPE)
endfunction()

# The literals are the values the removed pow calls computed:
# pow(10.0, -10.0 / 10.0), pow(10.0, -20.0 / 10.0), and pow(10.0, (-70.0 + 0.691) / 10.0).
replace_line("static double relative_gate = -10.0;" "")
replace_line("static double relative_gate_factor;" "static const double relative_gate_factor = 0.1;")
replace_line("static double minus_twenty_decibels;" "static const double minus_twenty_decibels = 0.01;")
replace_line(
    "static double histogram_energy_boundaries[1001];"
    "static double histogram_energy_boundaries[1001] = { 1.1724653045822981e-07 };"
)
replace_line("  relative_gate_factor = pow(10.0, relative_gate / 10.0);" "")
replace_line("  minus_twenty_decibels = pow(10.0, -20.0 / 10.0);" "")
replace_line("  histogram_energy_boundaries[0] = pow(10.0, (-70.0 + 0.691) / 10.0);" "")

file(WRITE "${EBUR128_SOURCE}" "${source}")
