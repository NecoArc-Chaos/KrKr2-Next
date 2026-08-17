struct Live2DRenderTarget {
    unsigned int fbo = 0;
    int width = 0;
    int height = 0;
};
Live2DRenderTarget g_live2dRenderTarget;

extern "C" void TVPRegisterKrkrLive2DPluginAnchor() {}
