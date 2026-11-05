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
    # This dependent target runs after the executable and every resource copy, including resource-only rebuilds.
    # It intentionally has no output stamp, since signing seals the complete mutable bundle.
    add_custom_target(
        ${target}Sign ALL
        COMMAND /bin/bash "${root}/common.sh" sign_mac_app_bundle "$<TARGET_BUNDLE_DIR:${target}>"
        DEPENDS ${target}
        VERBATIM
    )
endfunction()
