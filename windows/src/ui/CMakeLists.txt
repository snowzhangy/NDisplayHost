# WPF desktop shell using the .NET Framework that ships with Windows. No NuGet,
# WebView runtime or additional UI SDK is required.
set(WVD_FRAMEWORK "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319")
set(WVD_CSC "${WVD_FRAMEWORK}/csc.exe")
if(NOT EXISTS "${WVD_CSC}")
  message(WARNING ".NET Framework compiler unavailable; wvd-host UI is skipped")
  return()
endif()
set(WVD_UI_EXE "${CMAKE_CURRENT_BINARY_DIR}/wvd-host.exe")
# Name + version shown in the title bar, tray tooltip and activity log.
configure_file(Version.cs.in "${CMAKE_CURRENT_BINARY_DIR}/Version.cs" @ONLY)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/Version.cs" WVD_VERSION_CS)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/App.cs" WVD_APP_CS)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/HostController.cs" WVD_CONTROLLER_CS)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/TrayIcon.cs" WVD_TRAY_CS)
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/MainWindow.xaml" WVD_XAML)
add_custom_command(OUTPUT "${WVD_UI_EXE}"
  COMMAND "${WVD_CSC}" /nologo /target:winexe /platform:x64 /optimize+ /utf8output
    "/out:${WVD_UI_EXE}" /reference:System.dll /reference:System.Core.dll /reference:System.Xaml.dll
    /reference:System.Drawing.dll /reference:System.Windows.Forms.dll
    "/reference:${WVD_FRAMEWORK}/WPF/WindowsBase.dll"
    "/reference:${WVD_FRAMEWORK}/WPF/PresentationCore.dll"
    "/reference:${WVD_FRAMEWORK}/WPF/PresentationFramework.dll"
    "/resource:${WVD_XAML},WvdHost.MainWindow.xaml"
    "${WVD_APP_CS}" "${WVD_VERSION_CS}" "${WVD_CONTROLLER_CS}" "${WVD_TRAY_CS}"
  DEPENDS App.cs "${CMAKE_CURRENT_BINARY_DIR}/Version.cs" HostController.cs TrayIcon.cs
    MainWindow.xaml Version.cs.in
  VERBATIM)
add_custom_target(wvd-host ALL DEPENDS "${WVD_UI_EXE}")
add_dependencies(wvd-host wvd-media wvd-input wvd-diag wvd-testcard wvd-network)
