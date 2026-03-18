option(ORCASLICER_BUILD_IOS_UI_SHELL "Build iOS SwiftUI shell target" ON)

if (NOT ORCASLICER_BUILD_IOS_UI_SHELL)
    message(STATUS "iOS UI shell disabled (ORCASLICER_BUILD_IOS_UI_SHELL=OFF)")
    return()
endif ()

if (NOT APPLE)
    message(STATUS "Skipping iOS UI shell target on non-Apple host")
    return()
endif ()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" _orcaslicer_osx_sysroot_lower)
    if (_orcaslicer_osx_sysroot_lower MATCHES "iphonesimulator")
        set(_orcaslicer_swift_target_arch "${CMAKE_OSX_ARCHITECTURES}")
        if (_orcaslicer_swift_target_arch MATCHES ";")
            list(GET _orcaslicer_swift_target_arch 0 _orcaslicer_swift_target_arch)
        endif ()
        if (NOT _orcaslicer_swift_target_arch)
            set(_orcaslicer_swift_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif ()
        if (NOT _orcaslicer_swift_target_arch)
            set(_orcaslicer_swift_target_arch "arm64")
        endif ()

        set(_orcaslicer_swift_compiler_target
            "${_orcaslicer_swift_target_arch}-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator")
        set(CMAKE_Swift_COMPILER_TARGET
            "${_orcaslicer_swift_compiler_target}"
            CACHE STRING "Swift compiler target triple" FORCE)

        list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
            CMAKE_OSX_ARCHITECTURES
            CMAKE_OSX_DEPLOYMENT_TARGET
            CMAKE_OSX_SYSROOT
            CMAKE_Swift_COMPILER_TARGET
        )
        list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

        if (CMAKE_Swift_FLAGS_INIT)
            string(APPEND CMAKE_Swift_FLAGS_INIT " -target ${_orcaslicer_swift_compiler_target}")
        else ()
            set(CMAKE_Swift_FLAGS_INIT "-target ${_orcaslicer_swift_compiler_target}")
        endif ()

        message(STATUS "Using Swift compiler target for iOS simulator: ${CMAKE_Swift_COMPILER_TARGET}")
    endif ()
endif ()

enable_language(Swift)

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if (CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "16.0")
        message(FATAL_ERROR "OrcaSlicerIOS UI shell requires iOS deployment target 16.0 or newer (NavigationStack/presentationDetents usage).")
    endif ()
endif ()


set(ORCASLICER_IOS_SHELL_SOURCES
    App/OrcaSlicerIOSApp.swift
    App/RootViewportScreen.swift
    App/Overlay/FloatingActionButton.swift
    App/Overlay/FloatingControlOverlay.swift
    App/Panels/ProjectPanelView.swift
    App/Panels/ToolsPanelView.swift
    App/Panels/SliceSettingsPanelView.swift
    App/Panels/PrinterPanelView.swift
    App/Panels/ViewPanelView.swift
    App/Panels/AppSettingsPanelView.swift
    App/Panels/IOSFileActionSheets.swift
    App/State/AppSession.swift
    App/State/ViewportSession.swift
    App/State/PanelRouter.swift
    App/State/ToolPanelState.swift
    App/State/SliceSettingsState.swift
    App/State/MachineProfileState.swift
    App/State/ScreenshotLaunchConfiguration.swift
    App/State/ProjectFileAccessService.swift
    App/Viewport/MetalViewportContainer.swift
    App/OrcaSlicerIOS-Bridging-Header.h
    App/AppService/OrcaSlicerAppService.h
    App/AppService/OrcaSlicerAppService.mm
    App/Viewport/OrcaMetalViewportView.h
    App/Viewport/OrcaMetalViewportView.mm
    ../portability/app/SlicingOrchestrator.cpp
)

add_executable(OrcaSlicerIOS
    MACOSX_BUNDLE
    ${ORCASLICER_IOS_SHELL_SOURCES}
)

target_compile_features(OrcaSlicerIOS PRIVATE cxx_std_17)
set_target_properties(OrcaSlicerIOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-iOS.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.softfever3d.orcaslicer.ios"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_UISupportedInterfaceOrientations_iPhone "UIInterfaceOrientationPortrait"
    XCODE_ATTRIBUTE_INFOPLIST_KEY_UIRequiresFullScreen "YES"
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.9"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/App/OrcaSlicerIOS-Bridging-Header.h"
    # Prevent global C/C++ warning flags from being forwarded to SwiftDriver in Xcode mixed-language builds.
    XCODE_ATTRIBUTE_OTHER_SWIFT_FLAGS ""
    SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/App/OrcaSlicerIOS-Bridging-Header.h"
)

target_include_directories(OrcaSlicerIOS PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../)

target_link_libraries(OrcaSlicerIOS
    PRIVATE
        orcaslicer_portability_render_interface
        orcaslicer_portability_app_interface
        orcaslicer_render_ios_metal
        "-framework SwiftUI"
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
)

message(STATUS "Enabled iOS UI shell target: OrcaSlicerIOS")
