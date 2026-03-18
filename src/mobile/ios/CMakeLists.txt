option(ORCASLICER_BUILD_IOS_UI_SHELL "Build iOS SwiftUI shell target" ON)

if (NOT ORCASLICER_BUILD_IOS_UI_SHELL)
    message(STATUS "iOS UI shell disabled (ORCASLICER_BUILD_IOS_UI_SHELL=OFF)")
    return()
endif ()

if (NOT APPLE)
    message(STATUS "Skipping iOS UI shell target on non-Apple host")
    return()
endif ()

enable_language(Swift)

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
    App/State/AppSession.swift
    App/State/ViewportSession.swift
    App/State/PanelRouter.swift
    App/State/ToolPanelState.swift
    App/State/SliceSettingsState.swift
    App/State/MachineProfileState.swift
    App/Viewport/MetalViewportContainer.swift
    App/OrcaSlicerIOS-Bridging-Header.h
    App/Viewport/OrcaMetalViewportView.h
    App/Viewport/OrcaMetalViewportView.mm
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
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/App/OrcaSlicerIOS-Bridging-Header.h"
)

target_include_directories(OrcaSlicerIOS PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../)

target_link_libraries(OrcaSlicerIOS
    PRIVATE
        orcaslicer_portability_render_interface
        orcaslicer_render_ios_metal
        "-framework SwiftUI"
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
)

message(STATUS "Enabled iOS UI shell target: OrcaSlicerIOS")
