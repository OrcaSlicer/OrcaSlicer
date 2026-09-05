#ifndef slic3r_PluginVerifyRedirect_hpp_
#define slic3r_PluginVerifyRedirect_hpp_

namespace Slic3r {

// Windows-only. When running a forked/unsigned BambuStudio under a GENUINE,
// version-matched bambu-studio.exe launcher, the network plugin's "signed
// studio" gate still verifies *this* BambuStudio.dll via WinVerifyTrust and
// rejects it (unsigned). This installs in-process inline hooks (MinHook) so the
// plugin's Authenticode verification of our DLL resolves to the genuine official
// BambuStudio.dll instead, satisfying the gate so control commands get signed.
//
// MUST be called before the network plugin is loaded.
void install_plugin_verify_redirect();

} // namespace Slic3r

#endif
