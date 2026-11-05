# Final local signing for macOS app bundles, shared by Ninja and Xcode builds.
# Keep Xcode's automatic signing disabled so it cannot replace the explicit local signature.
function(audiobatch_sign_mac_app target)
    if(NOT APPLE)
        return()
    endif()

    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set_target_properties(
        ${target} PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
    )
    # A changed signing policy must re-run the final link/sign step in incremental Ninja builds too.
    set_property(
        TARGET ${target}
        APPEND
        PROPERTY LINK_DEPENDS "${root}/common.sh" "${root}/assets/macos.entitlements"
    )
    # Register this after JUCE's icon and resource steps. Nothing may modify the bundle afterwards.
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND /bin/bash "${root}/common.sh" sign_mac_app_bundle "$<TARGET_BUNDLE_DIR:${target}>"
        VERBATIM
    )
endfunction()
