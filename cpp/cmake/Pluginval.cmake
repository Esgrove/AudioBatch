# Validates a VST3 plugin with pluginval, https://github.com/Tracktion/pluginval
# Shared by the ImageControl and test plugin projects, which are built on their own.
#
# Off by default. Turn it on with -DAUDIOBATCH_PLUGINVAL=ON, then build the <plugin target>_pluginval target.
#
# The target runs RunPluginval.cmake, which checks out pluginval at the pinned commit into the repository's .deps directory,
# builds it there as a project of its own, and runs it on the plugin.
# The checkout and the pluginval executable are shared by all the projects, so they are downloaded and built once.
# pluginval is not added to the plugin projects with add_subdirectory or FetchContent, on purpose:
#  - It is built against its own pinned JUCE, which it fetches,
#    because its GUI code needs the JUCE version it was written for.
#  - It does not pick up the global compiler flags of the plugin projects.
#  - FetchContent keeps its download state in every build directory and would fetch the same source for each project.
#  - Nothing is downloaded or configured unless the validation target is built.
# pluginval is built without VST2 support: it only enables VST2 when the VST2_SDK_DIR environment variable is set,
# so that is cleared.
#
# Usage, after the plugin is added:
#   include("${CMAKE_CURRENT_SOURCE_DIR}/../cpp/cmake/Pluginval.cmake")
#   audiobatch_add_pluginval(MyPlugin)    # for the MyPlugin_VST3 target

include_guard(GLOBAL)

option(AUDIOBATCH_PLUGINVAL "Add targets that build pluginval and validate the VST3 plugins with it" OFF)
set(AUDIOBATCH_PLUGINVAL_STRICTNESS 5)

set(AUDIOBATCH_PLUGINVAL_REPOSITORY "https://github.com/Tracktion/pluginval.git")
# A commit of the develop branch.
set(AUDIOBATCH_PLUGINVAL_COMMIT "286684f72062250fd2494681b7ebdcb721f09e0d")
get_filename_component(AUDIOBATCH_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.deps" ABSOLUTE)

function(audiobatch_add_pluginval plugin_target)
    if(NOT AUDIOBATCH_PLUGINVAL)
        return()
    endif()
    if(NOT TARGET ${plugin_target}_VST3)
        message(FATAL_ERROR "audiobatch_add_pluginval: no VST3 target ${plugin_target}_VST3")
    endif()

    # JUCE stores the artefact path as a generator expression itself, so it is evaluated once more.
    set(artefact
        "$<TARGET_GENEX_EVAL:${plugin_target}_VST3,$<TARGET_PROPERTY:${plugin_target}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
    )
    add_custom_target(
        ${plugin_target}_pluginval
        COMMAND
            ${CMAKE_COMMAND} "-DDEPS_DIR=${AUDIOBATCH_DEPS_DIR}" "-DREPOSITORY=${AUDIOBATCH_PLUGINVAL_REPOSITORY}"
            "-DCOMMIT=${AUDIOBATCH_PLUGINVAL_COMMIT}" "-DSTRICTNESS=${AUDIOBATCH_PLUGINVAL_STRICTNESS}"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLUGIN=${artefact}" -P
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunPluginval.cmake"
        DEPENDS ${plugin_target}_VST3
        COMMENT "Validating ${plugin_target} with pluginval"
        USES_TERMINAL VERBATIM
    )
endfunction()
