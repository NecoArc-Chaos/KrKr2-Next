// Stub for TVPRegisterKrkrGLESPluginAnchor when the krkrgles plugin is not
// built (Live2D SDK is absent). engine_api.cpp calls this to force the linker
// to include the GLES plugin translation unit; the no-op stub satisfies the
// external reference without pulling in the real plugin code.
extern "C" void TVPRegisterKrkrGLESPluginAnchor() {}
