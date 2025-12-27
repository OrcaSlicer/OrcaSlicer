cmake_minimum_required(VERSION 3.13)
project(libslic3r_gui)

include(PrecompiledHeader)

set(SLIC3R_GUI_SOURCES
    GUI/Widgets/AnimaController.hpp
    GUI/Widgets/AnimaController.cpp
    Config/Snapshot.cpp
    Config/Snapshot.hpp
    Config/Version.cpp
    Config/Version.hpp
    GUI/2DBed.cpp
    GUI/2DBed.hpp
    GUI/3DBed.cpp
    GUI/3DBed.hpp
    GUI/Widgets/StaticGroup.cpp
    GUI/Widgets/StaticGroup.hpp
    GUI/3DScene.cpp
    GUI/3DScene.hpp
    GUI/Widgets/FilamentLoad.cpp
    GUI/Widgets/FilamentLoad.hpp
    GUI/AboutDialog.cpp
    GUI/AboutDialog.hpp
    GUI/AmsMappingPopup.cpp
    GUI/AmsMappingPopup.hpp
    GUI/AMSMaterialsSetting.cpp
    GUI/AMSMaterialsSetting.hpp
    GUI/AMSSetting.cpp
    GUI/AMSSetting.hpp
    GUI/AmsWidgets.cpp
    GUI/AmsWidgets.hpp
    GUI/Auxiliary.cpp
    GUI/AuxiliaryDataViewModel.cpp
    GUI/AuxiliaryDataViewModel.hpp
    GUI/AuxiliaryDialog.cpp
    GUI/AuxiliaryDialog.hpp
    GUI/Auxiliary.hpp
    GUI/BackgroundSlicingProcess.cpp
    GUI/BackgroundSlicingProcess.hpp
    GUI/BBLStatusBarBind.cpp
    GUI/BBLStatusBarBind.hpp
    GUI/BBLStatusBar.cpp
    GUI/BBLStatusBar.hpp
    GUI/BBLStatusBarSend.cpp
    GUI/BBLStatusBarSend.hpp
    GUI/BBLTopbar.cpp
    GUI/BBLTopbar.hpp
    GUI/BedShapeDialog.cpp
    GUI/BedShapeDialog.hpp
    GUI/BindDialog.cpp
    GUI/BindDialog.hpp
    GUI/BitmapCache.cpp
    GUI/BitmapCache.hpp
    GUI/BitmapComboBox.cpp
    GUI/BitmapComboBox.hpp
    GUI/BonjourDialog.cpp
    GUI/BonjourDialog.hpp
    GUI/calib_dlg.cpp
    GUI/calib_dlg.hpp
    GUI/Calibration.cpp
    GUI/Calibration.hpp
    GUI/CalibrationPanel.cpp
    GUI/CalibrationPanel.hpp
    GUI/CalibrationWizardCaliPage.cpp
    GUI/CalibrationWizardCaliPage.hpp
    GUI/CalibrationWizard.cpp
    GUI/CalibrationWizard.hpp
    GUI/CalibrationWizardPage.cpp
    GUI/CalibrationWizardPage.hpp
    GUI/CalibrationWizardPresetPage.cpp
    GUI/CalibrationWizardPresetPage.hpp
    GUI/CalibrationWizardSavePage.cpp
    GUI/CalibrationWizardSavePage.hpp
    GUI/CalibrationWizardStartPage.cpp
    GUI/CalibrationWizardStartPage.hpp
    GUI/CaliHistoryDialog.cpp
    GUI/CaliHistoryDialog.hpp
    GUI/Camera.cpp
    GUI/Camera.hpp
    GUI/CameraPopup.cpp
    GUI/CameraPopup.hpp
    GUI/CameraUtils.cpp
    GUI/CameraUtils.hpp
    GUI/CloneDialog.cpp
    GUI/CloneDialog.hpp
    GUI/ConfigManipulation.cpp
    GUI/ConfigManipulation.hpp
    GUI/ConfigWizard.cpp
    GUI/ConfigWizard.hpp
    GUI/ConfigWizard_private.hpp
    GUI/ConnectPrinter.cpp
    GUI/ConnectPrinter.hpp
    GUI/CreatePresetsDialog.cpp
    GUI/CreatePresetsDialog.hpp
    GUI/DailyTips.cpp
    GUI/DailyTips.hpp
    GUI/DesktopIntegrationDialog.cpp
    GUI/DesktopIntegrationDialog.hpp
    GUI/DeviceManager.cpp
    GUI/DeviceManager.hpp
    GUI/Downloader.cpp
    GUI/DownloaderFileGet.cpp
    GUI/DownloaderFileGet.hpp
    GUI/Downloader.hpp
    GUI/DownloadProgressDialog.cpp
    GUI/DownloadProgressDialog.hpp
    GUI/DragCanvas.cpp
    GUI/DragCanvas.hpp
    GUI/EditGCodeDialog.cpp
    GUI/EditGCodeDialog.hpp
    GUI/EncodedFilament.hpp
    GUI/EncodedFilament.cpp
    GUI/ExtraRenderers.cpp
    GUI/ExtraRenderers.hpp
    GUI/ExtrusionCalibration.cpp
    GUI/ExtrusionCalibration.hpp
    GUI/Field.cpp
    GUI/Field.hpp
    GUI/FileArchiveDialog.cpp
    GUI/FileArchiveDialog.hpp
    GUI/format.hpp
    GUI/GCodeViewer.cpp
    GUI/GCodeViewer.hpp
    GUI/Gizmos/GizmoObjectManipulation.cpp
    GUI/Gizmos/GizmoObjectManipulation.hpp
    GUI/Gizmos/GLGizmoAssembly.cpp
    GUI/Gizmos/GLGizmoAssembly.hpp
    GUI/Gizmos/GLGizmoBase.cpp
    GUI/Gizmos/GLGizmoBase.hpp
    GUI/Gizmos/GLGizmoBrimEars.cpp
    GUI/Gizmos/GLGizmoBrimEars.hpp
    GUI/Gizmos/GLGizmoCut.cpp
    GUI/Gizmos/GLGizmoCut.hpp
    GUI/Gizmos/GLGizmoEmboss.cpp
    GUI/Gizmos/GLGizmoEmboss.hpp
    #GUI/Gizmos/GLGizmoFaceDetector.cpp
    #GUI/Gizmos/GLGizmoFaceDetector.hpp
    GUI/Gizmos/GLGizmoFdmSupports.cpp
    GUI/Gizmos/GLGizmoFdmSupports.hpp
    GUI/Gizmos/GLGizmoFlatten.cpp
    GUI/Gizmos/GLGizmoFlatten.hpp
    GUI/Gizmos/GLGizmoFuzzySkin.cpp
    GUI/Gizmos/GLGizmoFuzzySkin.hpp
    #GUI/Gizmos/GLGizmoHollow.cpp
    #GUI/Gizmos/GLGizmoHollow.hpp
    GUI/Gizmos/GLGizmoMeasure.cpp
    GUI/Gizmos/GLGizmoMeasure.hpp
    GUI/Gizmos/GLGizmoMeshBoolean.cpp
    GUI/Gizmos/GLGizmoMeshBoolean.hpp
    GUI/Gizmos/GLGizmoMmuSegmentation.cpp
    GUI/Gizmos/GLGizmoMmuSegmentation.hpp
    GUI/Gizmos/GLGizmoMove.cpp
    GUI/Gizmos/GLGizmoMove.hpp
    GUI/Gizmos/GLGizmoPainterBase.cpp
    GUI/Gizmos/GLGizmoPainterBase.hpp
    GUI/Gizmos/GLGizmoRotate.cpp
    GUI/Gizmos/GLGizmoRotate.hpp
    GUI/Gizmos/GLGizmoScale.cpp
    GUI/Gizmos/GLGizmoScale.hpp
    GUI/Gizmos/GLGizmosCommon.cpp
    GUI/Gizmos/GLGizmosCommon.hpp
    GUI/Gizmos/GLGizmoSeam.cpp
    GUI/Gizmos/GLGizmoSeam.hpp
    GUI/Gizmos/GLGizmoSimplify.cpp
    GUI/Gizmos/GLGizmoSimplify.hpp
    #GUI/Gizmos/GLGizmoSlaSupports.cpp
    #GUI/Gizmos/GLGizmoSlaSupports.hpp
    GUI/Gizmos/GLGizmosManager.cpp
    GUI/Gizmos/GLGizmosManager.hpp
    GUI/Gizmos/GLGizmoSVG.cpp
    GUI/Gizmos/GLGizmoSVG.hpp
    #GUI/Gizmos/GLGizmoText.cpp
    #GUI/Gizmos/GLGizmoText.hpp
    GUI/GLCanvas3D.cpp
    GUI/GLCanvas3D.hpp
    GUI/GLModel.cpp
    GUI/GLModel.hpp
    GUI/GLSelectionRectangle.cpp
    GUI/GLSelectionRectangle.hpp
    GUI/GLShader.cpp
    GUI/GLShader.hpp
    GUI/GLShadersManager.cpp
    GUI/GLShadersManager.hpp
    GUI/GLTexture.cpp
    GUI/GLTexture.hpp
    GUI/GLToolbar.cpp
    GUI/GuiColor.cpp
    GUI/GuiColor.hpp
    GUI/GLToolbar.hpp
    GUI/ImageDPIFrame.cpp
    GUI/ImageDPIFrame.hpp
    GUI/GUI_App.cpp
    GUI/GUI_App.hpp
    GUI/GUI_AuxiliaryList.cpp
    GUI/GUI_AuxiliaryList.hpp
    GUI/GUI_Colors.cpp
    GUI/GUI_Colors.hpp
    GUI/GUI.cpp
    GUI/GUI_Factories.cpp
    GUI/GUI_Factories.hpp
    GUI/GUI_Geometry.cpp
    GUI/GUI_Geometry.hpp
    GUI/GUI.hpp
    GUI/GUI_Init.cpp
    GUI/GUI_Init.hpp
    GUI/GUI_ObjectLayers.cpp
    GUI/GUI_ObjectLayers.hpp
    GUI/GUI_ObjectList.cpp
    GUI/GUI_ObjectList.hpp
    GUI/GUI_ObjectSettings.cpp
    GUI/GUI_ObjectSettings.hpp
    GUI/GUI_ObjectTable.cpp
    GUI/GUI_ObjectTable.hpp
    GUI/GUI_ObjectTableSettings.cpp
    GUI/GUI_ObjectTableSettings.hpp
    GUI/GUI_Preview.cpp
    GUI/GUI_Preview.hpp
    GUI/GUI_Utils.cpp
    GUI/GUI_Utils.hpp
    GUI/HintNotification.cpp
    GUI/HintNotification.hpp
    GUI/ThermalPreconditioningDialog.cpp
    GUI/ThermalPreconditioningDialog.hpp
    GUI/HMS.cpp
    GUI/HMS.hpp
    GUI/HMSPanel.cpp
    GUI/HMSPanel.hpp
    GUI/HttpServer.cpp
    GUI/HttpServer.hpp
    GUI/I18N.cpp
    GUI/I18N.hpp
    GUI/DragDropPanel.cpp
    GUI/DragDropPanel.hpp
    GUI/CapsuleButton.cpp
    GUI/CapsuleButton.hpp
    GUI/FilamentMapPanel.cpp
    GUI/FilamentMapPanel.hpp
    GUI/FilamentMapDialog.cpp
    GUI/FilamentMapDialog.hpp
    GUI/IconManager.cpp
    GUI/IconManager.hpp
    GUI/ImageGrid.cpp
    GUI/ImageGrid.h
    GUI/ImGuiWrapper.cpp
    GUI/FilamentPickerDialog.cpp
    GUI/FilamentPickerDialog.hpp
    GUI/FilamentBitmapUtils.cpp
    GUI/FilamentBitmapUtils.hpp
    GUI/BaseTransparentDPIFrame.cpp
    GUI/BaseTransparentDPIFrame.hpp
    GUI/SyncAmsInfoDialog.cpp
    GUI/SyncAmsInfoDialog.hpp
    GUI/ImGuiWrapper.hpp
    GUI/IMSlider.cpp
    GUI/IMSlider.hpp
    GUI/DeviceErrorDialog.hpp
    GUI/DeviceErrorDialog.cpp
    GUI/IMToolbar.cpp
    GUI/IMToolbar.hpp
    GUI/InstanceCheck.cpp
    GUI/InstanceCheck.hpp
    GUI/Jobs/ArrangeJob.cpp
    GUI/Jobs/ArrangeJob.hpp
    GUI/Jobs/BindJob.cpp
    GUI/Jobs/BindJob.hpp
    GUI/Jobs/BoostThreadWorker.cpp
    GUI/Jobs/BoostThreadWorker.hpp
    GUI/Jobs/BusyCursorJob.hpp
    GUI/Jobs/CreateFontNameImageJob.cpp
    GUI/Jobs/CreateFontNameImageJob.hpp
    GUI/Jobs/CreateFontStyleImagesJob.cpp
    GUI/Jobs/CreateFontStyleImagesJob.hpp
    GUI/Jobs/EmbossJob.cpp
    GUI/Jobs/EmbossJob.hpp
    GUI/Jobs/FillBedJob.cpp
    GUI/Jobs/FillBedJob.hpp
    GUI/Jobs/Job.hpp
    GUI/Jobs/NotificationProgressIndicator.cpp
    GUI/Jobs/NotificationProgressIndicator.hpp
    GUI/Jobs/OAuthJob.cpp
    GUI/Jobs/OAuthJob.hpp
    GUI/Jobs/OrientJob.cpp
    GUI/Jobs/OrientJob.hpp
    GUI/Jobs/PlaterWorker.hpp
    GUI/Jobs/PrintJob.cpp
    GUI/Jobs/PrintJob.hpp
    GUI/Jobs/ProgressIndicator.hpp
    GUI/Jobs/RotoptimizeJob.cpp
    GUI/Jobs/RotoptimizeJob.hpp
    GUI/Jobs/SendJob.cpp
    GUI/Jobs/SendJob.hpp
    GUI/Jobs/SLAImportDialog.hpp
    GUI/Jobs/SLAImportJob.cpp
    GUI/Jobs/SLAImportJob.hpp
    GUI/Jobs/ThreadSafeQueue.hpp
    GUI/Jobs/UpgradeNetworkJob.cpp
    GUI/Jobs/UpgradeNetworkJob.hpp
    GUI/Jobs/Worker.hpp
    GUI/KBShortcutsDialog.cpp
    GUI/KBShortcutsDialog.hpp
    GUI/MainFrame.cpp
    GUI/MainFrame.hpp
    GUI/MarkdownTip.cpp
    GUI/MarkdownTip.hpp
    GUI/MediaFilePanel.cpp
    GUI/MediaFilePanel.h
    GUI/MediaPlayCtrl.cpp
    GUI/MediaPlayCtrl.h
    GUI/MeshUtils.cpp
    GUI/MeshUtils.hpp
    GUI/ModelMall.cpp
    GUI/ModelMall.hpp
    GUI/MonitorBasePanel.cpp
    GUI/BBLStatusBarPrint.hpp
    GUI/BBLStatusBarPrint.cpp
    GUI/MonitorBasePanel.h
    GUI/Monitor.cpp
    GUI/Monitor.hpp
    GUI/MonitorPage.cpp
    GUI/MonitorPage.hpp
    GUI/Mouse3DController.cpp
    GUI/Mouse3DController.hpp
    GUI/MsgDialog.cpp
    GUI/MsgDialog.hpp
    GUI/MultiMachine.cpp
    GUI/MultiMachine.hpp
    GUI/MultiMachineManagerPage.cpp
    GUI/MultiMachineManagerPage.hpp
    GUI/MultiMachinePage.cpp
    GUI/MultiMachinePage.hpp
    GUI/MultiPrintJob.cpp
    GUI/MultiPrintJob.hpp
    GUI/MultiSendMachineModel.cpp
    GUI/MultiSendMachineModel.hpp
    GUI/MultiTaskManagerPage.cpp
    GUI/MultiTaskManagerPage.hpp
    GUI/MultiTaskModel.cpp
    GUI/MultiTaskModel.hpp
    GUI/NetworkTestDialog.cpp
    GUI/NetworkTestDialog.hpp
    GUI/Notebook.cpp
    GUI/Notebook.hpp
    GUI/NotificationManager.cpp
    GUI/NotificationManager.hpp
    GUI/OAuthDialog.cpp
    GUI/OAuthDialog.hpp
    GUI/ObjColorDialog.cpp
    GUI/ObjColorDialog.hpp
    GUI/ObjectDataViewModel.cpp
    GUI/ObjectDataViewModel.hpp
    GUI/OG_CustomCtrl.cpp
    GUI/OG_CustomCtrl.hpp
    GUI/OpenGLManager.cpp
    GUI/OpenGLManager.hpp
    GUI/OptionsGroup.cpp
    GUI/OptionsGroup.hpp
    GUI/ParamsDialog.cpp
    GUI/ParamsDialog.hpp
    GUI/ParamsPanel.cpp
    GUI/ParamsPanel.hpp
    GUI/PartPlate.cpp
    GUI/PartPlate.hpp
    GUI/FilamentGroupPopup.hpp
    GUI/FilamentGroupPopup.cpp
    GUI/PhysicalPrinterDialog.cpp
    GUI/PhysicalPrinterDialog.hpp
    GUI/Plater.cpp
    GUI/Plater.hpp
    GUI/PlateSettingsDialog.cpp
    GUI/PlateSettingsDialog.hpp
    GUI/Preferences.cpp
    GUI/Preferences.hpp
    GUI/PresetComboBoxes.cpp
    GUI/PresetComboBoxes.hpp
    GUI/PresetHints.cpp
    GUI/PresetHints.hpp
    GUI/PrinterCloudAuthDialog.cpp
    GUI/PrinterCloudAuthDialog.hpp
    GUI/Printer/PrinterFileSystem.cpp
    GUI/Printer/PrinterFileSystem.h
    GUI/PrinterWebView.cpp
    GUI/PrinterWebView.hpp
    GUI/PrintHostDialogs.cpp
    GUI/PrintHostDialogs.hpp
    GUI/PrintOptionsDialog.cpp
    GUI/PrintOptionsDialog.hpp
    GUI/PrivacyUpdateDialog.cpp
    GUI/PrivacyUpdateDialog.hpp
    GUI/ProgressStatusBar.cpp
    GUI/ProgressStatusBar.hpp
    GUI/Project.cpp
    GUI/ProjectDirtyStateManager.cpp
    GUI/ProjectDirtyStateManager.hpp
    GUI/Project.hpp
    GUI/PublishDialog.cpp
    GUI/PublishDialog.hpp
    GUI/RammingChart.cpp
    GUI/RammingChart.hpp
    GUI/RecenterDialog.cpp
    GUI/RecenterDialog.hpp
    GUI/ReleaseNote.cpp
    GUI/ReleaseNote.hpp
    GUI/RemovableDriveManager.cpp
    GUI/RemovableDriveManager.hpp
    GUI/SavePresetDialog.cpp
    GUI/SavePresetDialog.hpp
    GUI/SceneRaycaster.cpp
    GUI/SceneRaycaster.hpp
    GUI/PartSkipCommon.hpp
    GUI/PartSkipDialog.cpp
    GUI/PartSkipDialog.hpp
    GUI/SkipPartCanvas.cpp
    GUI/SkipPartCanvas.hpp
    GUI/Search.cpp
    GUI/Search.hpp
    GUI/Selection.cpp
    GUI/Selection.hpp
    GUI/SelectMachine.cpp
    GUI/SelectMachine.hpp
    GUI/PrePrintChecker.hpp
    GUI/PrePrintChecker.cpp
    GUI/SelectMachinePop.cpp
    GUI/SelectMachinePop.hpp
    GUI/SendMultiMachinePage.cpp
    GUI/SendMultiMachinePage.hpp
    GUI/SendSystemInfoDialog.cpp
    GUI/SendSystemInfoDialog.hpp
    GUI/SendToPrinter.cpp
    GUI/SendToPrinter.hpp
    GUI/SingleChoiceDialog.cpp
    GUI/SingleChoiceDialog.hpp
    GUI/SliceInfoPanel.cpp
    GUI/SliceInfoPanel.hpp
    GUI/SlicingProgressNotification.cpp
    GUI/SlicingProgressNotification.hpp
    GUI/StatusPanel.cpp
    GUI/StatusPanel.hpp
    GUI/StepMeshDialog.cpp
    GUI/StepMeshDialog.hpp
    GUI/SurfaceDrag.cpp
    GUI/SurfaceDrag.hpp
    GUI/Tabbook.cpp
    GUI/Tabbook.hpp
    GUI/TabButton.cpp
    GUI/TabButton.hpp
    GUI/Tab.cpp
    GUI/Tab.hpp
    GUI/TaskManager.cpp
    GUI/TaskManager.hpp
    GUI/TextLines.cpp
    GUI/TextLines.hpp
    GUI/TickCode.cpp
    GUI/TickCode.hpp
    GUI/UnsavedChangesDialog.cpp
    GUI/UnsavedChangesDialog.hpp
    GUI/UpdateDialogs.cpp
    GUI/UpdateDialogs.hpp
    GUI/UpgradePanel.cpp
    GUI/UpgradePanel.hpp
    GUI/UserManager.cpp
    GUI/UserManager.hpp
    GUI/UserNotification.cpp
    GUI/UserNotification.hpp
    GUI/WebDownPluginDlg.cpp
    GUI/WebDownPluginDlg.hpp
    GUI/WebGuideDialog.cpp
    GUI/WebGuideDialog.hpp
    GUI/WebUserLoginDialog.cpp
    GUI/WebUserLoginDialog.hpp
    GUI/WebViewDialog.cpp
    GUI/WebViewDialog.hpp
    GUI/Widgets/AMSControl.cpp
    GUI/Widgets/AMSControl.hpp
    GUI/Widgets/AMSItem.cpp
    GUI/Widgets/AMSItem.hpp
    GUI/Widgets/AxisCtrlButton.cpp
    GUI/Widgets/AxisCtrlButton.hpp
    GUI/SafetyOptionsDialog.hpp
    GUI/SafetyOptionsDialog.cpp
    GUI/Widgets/Button.cpp
    GUI/Widgets/Button.hpp
    GUI/Widgets/CheckBox.cpp
    GUI/Widgets/CheckBox.hpp
    GUI/Widgets/ComboBox.cpp
    GUI/Widgets/ComboBox.hpp
    GUI/Widgets/DialogButtons.cpp
    GUI/Widgets/DialogButtons.hpp
    GUI/Widgets/DropDown.cpp
    GUI/Widgets/DropDown.hpp
    GUI/Widgets/ErrorMsgStaticText.cpp
    GUI/Widgets/ErrorMsgStaticText.hpp
    GUI/Widgets/FanControl.cpp
    GUI/Widgets/FanControl.hpp
    GUI/Widgets/ImageSwitchButton.cpp
    GUI/Widgets/ImageSwitchButton.hpp
    GUI/Widgets/Label.cpp
    GUI/Widgets/LabeledStaticBox.cpp
    GUI/Widgets/LabeledStaticBox.hpp
    GUI/Widgets/Label.hpp
    GUI/Widgets/PopupWindow.cpp
    GUI/Widgets/PopupWindow.hpp
    GUI/Widgets/ProgressBar.cpp
    GUI/Widgets/ProgressBar.hpp
    GUI/Widgets/ProgressDialog.cpp
    GUI/Widgets/ProgressDialog.hpp
    GUI/Widgets/RadioBox.cpp
    GUI/Widgets/RadioBox.hpp
    GUI/Widgets/RadioGroup.cpp
    GUI/Widgets/RadioGroup.hpp
    GUI/Widgets/RoundedRectangle.cpp
    GUI/Widgets/RoundedRectangle.hpp
    GUI/Widgets/Scrollbar.cpp
    GUI/Widgets/Scrollbar.hpp
    GUI/Widgets/ScrolledWindow.cpp
    GUI/Widgets/ScrolledWindow.hpp
    GUI/Widgets/SideButton.cpp
    GUI/Widgets/SideButton.hpp
    GUI/Widgets/SideMenuPopup.cpp
    GUI/Widgets/SideMenuPopup.hpp
    GUI/Widgets/SideTools.cpp
    GUI/Widgets/SideTools.hpp
    GUI/Widgets/SpinInput.cpp
    GUI/Widgets/SpinInput.hpp
    GUI/Widgets/StateColor.cpp
    GUI/Widgets/StateColor.hpp
    GUI/Widgets/StateHandler.cpp
    GUI/Widgets/StateHandler.hpp
    GUI/Widgets/StaticBox.cpp
    GUI/Widgets/StaticBox.hpp
    GUI/Widgets/StaticLine.cpp
    GUI/Widgets/StaticLine.hpp
    GUI/Widgets/StepCtrl.cpp
    GUI/Widgets/StepCtrl.hpp
    GUI/Widgets/SwitchButton.cpp
    GUI/Widgets/SwitchButton.hpp
    GUI/Widgets/TabCtrl.cpp
    GUI/Widgets/TabCtrl.hpp
    GUI/Widgets/TempInput.cpp
    GUI/Widgets/TempInput.hpp
    GUI/Widgets/TextInput.cpp
    GUI/Widgets/TextInput.hpp
    GUI/Widgets/WebView.cpp
    GUI/Widgets/WebView.hpp
    GUI/WipeTowerDialog.cpp
    GUI/wxExtensions.cpp
    GUI/wxExtensions.hpp
    pchheader.cpp
    pchheader.hpp
    Utils/ASCIIFolding.cpp
    Utils/ASCIIFolding.hpp
    Utils/AstroBox.cpp
    Utils/AstroBox.hpp
    Utils/bambu_networking.hpp
    Utils/Bonjour.cpp
    Utils/Bonjour.hpp
    Utils/CalibUtils.cpp
    Utils/CalibUtils.hpp
    Utils/ColorSpaceConvert.cpp
    Utils/ColorSpaceConvert.hpp
    Utils/CrealityPrint.cpp
    Utils/CrealityPrint.hpp
    Utils/Duet.cpp
    Utils/Duet.hpp
    Utils/ElegooLink.cpp
    Utils/ElegooLink.hpp
    Utils/EmbossStyleManager.cpp
    Utils/EmbossStyleManager.hpp
    Utils/ESP3D.cpp
    Utils/ESP3D.hpp
    Utils/FileHelp.cpp
    Utils/FileHelp.hpp
    Utils/FixModelByWin10.cpp
    Utils/FixModelByWin10.hpp
    Utils/FlashAir.cpp
    Utils/FlashAir.hpp
    Utils/Flashforge.cpp
    Utils/Flashforge.hpp
    Utils/FontConfigHelp.cpp
    Utils/FontConfigHelp.hpp
    Utils/HexFile.cpp
    Utils/HexFile.hpp
    Utils/Http.cpp
    Utils/Http.hpp
    Utils/InstanceID.cpp
    Utils/InstanceID.hpp
    Utils/json_diff.cpp
    Utils/json_diff.hpp
    Utils/minilzo_extension.cpp
    Utils/minilzo_extension.hpp
    Utils/MKS.cpp
    Utils/MKS.hpp
    Utils/NetworkAgent.cpp
    Utils/NetworkAgent.hpp
    Utils/Obico.cpp
    Utils/Obico.hpp
    Utils/OctoPrint.cpp
    Utils/OctoPrint.hpp
    Utils/PresetUpdater.cpp
    Utils/PresetUpdater.hpp
    Utils/PrintHost.cpp
    Utils/PrintHost.hpp
    Utils/Process.cpp
    Utils/Process.hpp
    Utils/ProfileDescription.hpp
    Utils/Profile.hpp
    Utils/RaycastManager.cpp
    Utils/RaycastManager.hpp
    Utils/Repetier.cpp
    Utils/Repetier.hpp
    Utils/Serial.cpp
    Utils/Serial.hpp
    Utils/SerialMessage.hpp
    Utils/SerialMessageType.hpp
    Utils/SimplyPrint.cpp
    Utils/SimplyPrint.hpp
    Utils/TCPConsole.cpp
    Utils/TCPConsole.hpp
    Utils/UndoRedo.cpp
    Utils/UndoRedo.hpp
    Utils/WebSocketClient.hpp
    Utils/WxFontUtils.cpp
    Utils/WxFontUtils.hpp
    Utils/FileTransferUtils.cpp
    Utils/FileTransferUtils.hpp
)

add_subdirectory(GUI/DeviceCore)
add_subdirectory(GUI/DeviceTab)

if (WIN32)
    list(APPEND SLIC3R_GUI_SOURCES
            GUI/dark_mode/dark_mode.hpp
            GUI/dark_mode/IatHook.hpp
            GUI/dark_mode/UAHMenuBar.hpp
            GUI/dark_mode.hpp
            GUI/dark_mode.cpp
        )
endif ()

if (APPLE)
    list(APPEND SLIC3R_GUI_SOURCES
            Utils/RetinaHelperImpl.mm
            Utils/MacDarkMode.mm
            GUI/RemovableDriveManagerMM.mm
            GUI/RemovableDriveManagerMM.h
            GUI/Mouse3DHandlerMac.mm
            GUI/InstanceCheckMac.mm
            GUI/InstanceCheckMac.h
            GUI/GUI_UtilsMac.mm
            GUI/wxMediaCtrl2.mm
            GUI/wxMediaCtrl2.h
        )
    FIND_LIBRARY(DISKARBITRATION_LIBRARY DiskArbitration)
else ()
    list(APPEND SLIC3R_GUI_SOURCES
            GUI/wxMediaCtrl2.cpp
            GUI/wxMediaCtrl2.h
        )
endif ()

if (UNIX AND NOT APPLE)
    list(APPEND SLIC3R_GUI_SOURCES
        GUI/Printer/gstbambusrc.c
    )
endif ()

set(ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY}")
string(STRIP "${ORCA_UPDATER_SIG_KEY_B64}" ORCA_UPDATER_SIG_KEY_B64)
string(REPLACE "\n" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE "\r" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE "\t" "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")
string(REPLACE " "  "" ORCA_UPDATER_SIG_KEY_B64 "${ORCA_UPDATER_SIG_KEY_B64}")

set(ORCA_UPDATER_SIG_KEY_AVAILABLE 0)
if(ORCA_UPDATER_SIG_KEY_B64)
    set(ORCA_UPDATER_SIG_KEY_AVAILABLE 1)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/GeneratedConfig.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/GeneratedConfig.hpp
               @ONLY)

if (WIN32)
    add_library(libslic3r_gui STATIC
        ${SLIC3R_GUI_SOURCES}
        ../dev-utils/BaseException.cpp
        ../dev-utils/BaseException.h
        ../dev-utils/StackWalker.cpp
        ../dev-utils/StackWalker.h
    )
else ()
    add_library(libslic3r_gui STATIC ${SLIC3R_GUI_SOURCES})
endif ()
target_include_directories(libslic3r_gui PRIVATE Utils ${CMAKE_CURRENT_BINARY_DIR})

if (WIN32)
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../deps/WebView2/include)
    target_link_libraries(libslic3r_gui Advapi32)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SLIC3R_GUI_SOURCES})

encoding_check(libslic3r_gui)

target_link_libraries(libslic3r_gui libslic3r cereal::cereal imgui imguizmo minilzo GLEW::GLEW OpenGL::GL hidapi ${wxWidgets_LIBRARIES} glfw libcurl OpenSSL::SSL OpenSSL::Crypto noise::noise)

if (MSVC)
    target_link_libraries(libslic3r_gui Setupapi.lib)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FIND_LIBRARY(WAYLAND_SERVER_LIBRARIES NAMES wayland-server)
    FIND_LIBRARY(WAYLAND_EGL_LIBRARIES    NAMES wayland-egl)
    FIND_LIBRARY(WAYLAND_CLIENT_LIBRARIES NAMES wayland-client)
    find_package(CURL REQUIRED)
    target_link_libraries(libslic3r_gui
        OpenGL::EGL
        ${DBUS_LIBRARIES}
        ${WAYLAND_SERVER_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
        ${WAYLAND_CLIENT_LIBRARIES}
        ${CURL_LIBRARIES}
    )
elseif (APPLE)
    target_link_libraries(libslic3r_gui ${DISKARBITRATION_LIBRARY} "-framework Security")
endif()

if (SLIC3R_STATIC)
    # FIXME: This was previously exported by wx-config but the wxWidgets
    # cmake build forgets this and the build fails in debug mode (or on raspberry release)
    target_compile_definitions(libslic3r_gui PUBLIC -DwxDEBUG_LEVEL=0)
endif()

if (SPNAV_LIB)
    target_link_libraries(libslic3r_gui ${SPNAV_LIB})
endif()

if (SLIC3R_STATIC AND NOT SLIC3R_STATIC_EXCLUDE_CURL AND UNIX AND NOT APPLE)
    target_compile_definitions(libslic3r_gui PRIVATE OPENSSL_CERT_OVERRIDE)
endif ()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r_gui pchheader.hpp FORCEINCLUDE)
endif ()

# We need to implement some hacks for wxWidgets and touch the underlying GTK
# layer and sub-libraries. This forces us to use the include locations and
# link these libraries.
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    find_package(GTK${SLIC3R_GTK} REQUIRED)
    pkg_check_modules(LIBSECRET REQUIRED libsecret-1)
    if (FLATPAK)
        # I don't know why this is needed, but for whatever reason slic3r isn't
        # linking to X11 and webkit2gtk. force it.
        find_package(X11 REQUIRED)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(webkit2gtk REQUIRED webkit2gtk-4.1)
        target_link_libraries (libslic3r_gui ${X11_LIBRARIES} ${webkit2gtk_LIBRARIES})
    endif()
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${GTK${SLIC3R_GTK}_INCLUDE_DIRS} ${LIBSECRET_INCLUDE_DIRS})
    target_link_libraries(libslic3r_gui ${GTK${SLIC3R_GTK}_LIBRARIES} fontconfig ${LIBSECRET_LIBRARIES})


    # We add GStreamer for bambu:/// support.
    pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
    pkg_check_modules(GST_BASE REQUIRED gstreamer-base-1.0)
    target_link_libraries(libslic3r_gui ${GSTREAMER_LIBRARIES} ${GST_BASE_LIBRARIES})
    target_include_directories(libslic3r_gui SYSTEM PRIVATE ${GSTREAMER_INCLUDE_DIRS} ${GST_BASE_INCLUDE_DIRS})
endif ()

# Add a definition so that we can tell we are compiling slic3r.
target_compile_definitions(libslic3r_gui PRIVATE SLIC3R_CURRENTLY_COMPILING_GUI_MODULE)
