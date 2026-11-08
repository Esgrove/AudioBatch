# Checks out and builds pluginval in the shared .deps directory, then validates a plugin with it.
# Run by the <plugin target>_pluginval targets of Pluginval.cmake, as
#   cmake -DDEPS_DIR=... -DREPOSITORY=... -DCOMMIT=... -DSTRICTNESS=... -DGENERATOR=... -DPLUGIN=... -P RunPluginval.cmake

foreach(variable DEPS_DIR REPOSITORY COMMIT STRICTNESS GENERATOR PLUGIN)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "RunPluginval.cmake: ${variable} is not set")
    endif()
endforeach()

find_package(Git REQUIRED)
set(source_dir "${DEPS_DIR}/pluginval-src")
set(build_dir "${DEPS_DIR}/pluginval-build")

# The .deps directory is inside the AudioBatch repository, so git is told not to look any higher than the checkout.
set(ENV{GIT_CEILING_DIRECTORIES} "${DEPS_DIR}")

# Checks out the pinned commit, unless it is checked out already.
set(current_commit "")
if(EXISTS "${source_dir}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        OUTPUT_VARIABLE current_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
endif()
if(NOT current_commit STREQUAL COMMIT)
    message(STATUS "Fetching pluginval ${COMMIT} into ${source_dir}")
    file(REMOVE_RECURSE "${source_dir}")
    file(MAKE_DIRECTORY "${source_dir}")
    foreach(
        step "init" "remote;add;origin;${REPOSITORY}" "fetch;--depth;1;origin;${COMMIT}" "checkout;--detach;FETCH_HEAD"
    )
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" ${step}
            RESULT_VARIABLE result
            ERROR_VARIABLE error_output
            OUTPUT_QUIET
        )
        if(NOT result EQUAL 0)
            file(REMOVE_RECURSE "${source_dir}")
            message(FATAL_ERROR "pluginval checkout failed at 'git ${step}': ${error_output}")
        endif()
    endforeach()
endif()

# The dependencies pluginval fetches with CPM, including its JUCE, are downloaded once as well.
# VST2 support is only enabled by the VST2 SDK, so make sure it is not picked up.
set(ENV{CPM_SOURCE_CACHE} "${DEPS_DIR}/cpm")
unset(ENV{VST2_SDK_DIR})

# Configures again when the commit changed, which also covers a new checkout.
set(stamp "${build_dir}/commit.txt")
set(built_commit "")
if(EXISTS "${stamp}")
    file(READ "${stamp}" built_commit)
endif()
if(NOT built_commit STREQUAL COMMIT OR NOT EXISTS "${build_dir}/CMakeCache.txt")
    message(STATUS "Configuring pluginval in ${build_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE=Release
                COMMAND_ERROR_IS_FATAL ANY
    )
    file(WRITE "${stamp}" "${COMMIT}")
endif()

message(STATUS "Building pluginval")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release --target pluginval COMMAND_ERROR_IS_FATAL ANY
)

set(artefacts "${build_dir}/pluginval_artefacts/Release")
if(WIN32)
    set(pluginval "${artefacts}/pluginval.exe")
elseif(APPLE)
    set(pluginval "${artefacts}/pluginval.app/Contents/MacOS/pluginval")
else()
    set(pluginval "${artefacts}/pluginval")
endif()
if(NOT EXISTS "${pluginval}")
    message(FATAL_ERROR "pluginval was not found after the build: ${pluginval}")
endif()

message(STATUS "Validating ${PLUGIN} at strictness level ${STRICTNESS}")
execute_process(
    COMMAND "${pluginval}" validate --strictness-level "${STRICTNESS}" "${PLUGIN}" COMMAND_ERROR_IS_FATAL ANY
)
