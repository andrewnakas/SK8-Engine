# Rearrange the staged Vulkan files into a layout codesign will accept.
#
# rexglue_configure_target() stages libMoltenVK.dylib, any Vulkan loader and
# MoltenVK_icd.json next to the executable. That is right for a bare binary and
# wrong for a bundle: codesign refuses a bundle with non-code files in
# Contents/MacOS, failing with "In subcomponent: .../MoltenVK_icd.json". Apple's
# layout wants code in Contents/Frameworks and data in Contents/Resources.
#
# Run as a POST_BUILD step AFTER the SDK's staging (which is added earlier, so
# it runs earlier).
#
# Required: BUNDLE_CONTENTS
if(NOT DEFINED BUNDLE_CONTENTS OR BUNDLE_CONTENTS STREQUAL "")
    message(FATAL_ERROR "BUNDLE_CONTENTS must be set")
endif()

set(_macos "${BUNDLE_CONTENTS}/MacOS")
set(_frameworks "${BUNDLE_CONTENTS}/Frameworks")
set(_resources "${BUNDLE_CONTENTS}/Resources")
file(MAKE_DIRECTORY "${_frameworks}" "${_resources}")

# Code -> Frameworks.
file(GLOB _dylibs "${_macos}/*.dylib")
foreach(_dylib ${_dylibs})
    get_filename_component(_name "${_dylib}" NAME)
    file(RENAME "${_dylib}" "${_frameworks}/${_name}")
    message(STATUS "bundle: ${_name} -> Contents/Frameworks")
endforeach()

# Data -> Resources. The ICD's library_path is relative to the JSON, so it has
# to be repointed now that the two no longer sit side by side.
if(EXISTS "${_macos}/MoltenVK_icd.json")
    file(REMOVE "${_macos}/MoltenVK_icd.json")
endif()
file(WRITE "${_resources}/MoltenVK_icd.json" [=[
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../Frameworks/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
]=])
message(STATUS "bundle: MoltenVK_icd.json -> Contents/Resources (library_path repointed)")
