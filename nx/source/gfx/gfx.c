#include <string.h>
#include <switch.h>

static bool g_gfxInitialized = 0;
static ViDisplay g_gfxDisplay;
static Handle g_gfxDisplayVsyncEvent = INVALID_HANDLE;
static ViLayer g_gfxLayer;
static u8 g_gfxNativeWindow[0x100];
static u64 g_gfxNativeWindow_Size;
static s32 g_gfxNativeWindow_ID;
static Binder g_gfxBinderSession;
static s32 g_gfxCurrentBuffer = 0;
static s32 g_gfxCurrentProducerBuffer = 0;
static bool g_gfx_ProducerConnected = 0;
static bool g_gfx_ProducerSlotsRequested[2] = {0, 0};
static u8 *g_gfxFramebuf;
static size_t g_gfxFramebufSize;
static bufferProducerFence g_gfx_DequeueBuffer_fence;
static bufferProducerQueueBufferOutput g_gfx_Connect_QueueBufferOutput;
static bufferProducerQueueBufferOutput g_gfx_QueueBuffer_QueueBufferOutput;

static bool g_gfxDoubleBuf = 1;

size_t g_gfx_framebuf_width=0, g_gfx_framebuf_aligned_width=0;
size_t g_gfx_framebuf_height=0, g_gfx_framebuf_aligned_height=0;
size_t g_gfx_framebuf_display_width=0, g_gfx_framebuf_display_height=0;
size_t g_gfx_singleframebuf_size=0;

static AppletHookCookie g_gfx_autoresolution_applethookcookie;
static bool g_gfx_autoresolution_enabled;

static s32 g_gfx_autoresolution_handheld_width;
static s32 g_gfx_autoresolution_handheld_height;
static s32 g_gfx_autoresolution_docked_width;
static s32 g_gfx_autoresolution_docked_height;

extern u32 __nx_applet_type;

extern u32 g_nvgfx_totalframebufs;
extern nvioctl_fence g_nvgfx_nvhostgpu_gpfifo_fence;

//static Result _gfxGetDisplayResolution(u64 *width, u64 *height);

//TODO: Implement support for non-720p width/height.

//TODO: Let the user configure some of this?
static bufferProducerQueueBufferInput g_gfxQueueBufferData = {
    .timestamp = 0x0,
    .isAutoTimestamp = 0x1,
    .crop = {0x0, 0x0, 0x0, 0x0}, //Official apps which use multiple resolutions configure this for the currently used resolution, depending on the current appletOperationMode.
    .scalingMode = 0x0,
    .transform = 0x2,
    .stickyTransform = 0x0,
    .unk = {0x0, 0x1},

    .fence = {
        .is_valid = 0x1,
        .nv_fences = {
            {
            .id = 0xffffffff, //Official sw sets this to the output fence from the last nvioctlChannel_SubmitGPFIFO().
            .value = 0x0,
            },
            {0xffffffff, 0x0}, {0xffffffff, 0x0}, {0xffffffff, 0x0},
        },
    }
};

//Some of this struct is based on tegra_dc_ext_flip_windowattr.
//TODO: How much of this struct do official apps really set? Most of it seems to be used as-is from the bufferProducerRequestBuffer() output.
static bufferProducerGraphicBuffer g_gfx_BufferInitData = {
    .magic = 0x47424652,//"RFBG"/'GBFR'
    .format = 0x1,
    .usage = 0xb00,

    .pid = 0x2a, //Official sw sets this to the output of "getpid()", which calls a func which is hard-coded for returning 0x2a.
    .refcount = 0x0,  //Official sw sets this to the output of "android_atomic_inc()".

    .numFds = 0x0,
    .numInts = sizeof(g_gfx_BufferInitData.data)>>2,//0x51

    .data = {
        .unk_x0 = 0xffffffff,
        .unk_x8 = 0x0,
        .unk_xc = 0xdaffcaff,
        .unk_x10 = 0x2a,
        .unk_x14 = 0x0,
        .unk_x18 = 0xb00,
        .unk_x1c = 0x1,
        .unk_x20 = 0x1,
        .unk_x2c = 0x1,
        .unk_x30 = 0x0,
        .flags = 0x532120,
        .unk_x40 = 0x1,
        .unk_x44 = 0x3,
        .unk_x54 = 0xfe,
        .unk_x58 = 0x4,
    }
};

static Result _gfxGetNativeWindowID(u8 *buf, u64 size, s32 *out_ID) {
    u32 *bufptr = (u32*)buf;

    //Validate ParcelData{Size|Offset}.
    if((u64)bufptr[1] > size || (u64)bufptr[0] > size || ((u64)bufptr[1])+((u64)bufptr[0]) > size) return MAKERESULT(MODULE_LIBNX, LIBNX_BADINPUT);
    if(bufptr[0] < 0xc) return MAKERESULT(MODULE_LIBNX, LIBNX_BADINPUT);
    //bufptr = start of ParcelData
    bufptr = (u32*)&buf[bufptr[1]];

    *out_ID = (s32)bufptr[2];

    return 0;
}

static Result _gfxDequeueBuffer(void) {
    Result rc=0;
    bufferProducerFence *fence = &g_gfx_DequeueBuffer_fence;
    bufferProducerFence tmp_fence;
    bool async=0;

    if (!g_gfxDoubleBuf) {
        g_gfxCurrentProducerBuffer = -1;
        return 0;
    }

    memcpy(&tmp_fence, fence, sizeof(bufferProducerFence));//Offical sw waits on the fence from the previous DequeueBuffer call. Using the fence from the current DequeueBuffer call results in nvgfxEventWait() failing.

    rc = bufferProducerDequeueBuffer(async, g_gfx_framebuf_width, g_gfx_framebuf_height, 0, 0x300, &g_gfxCurrentProducerBuffer, fence);

    if (R_SUCCEEDED(rc) && tmp_fence.is_valid) rc = nvgfxEventWait(tmp_fence.nv_fences[0].id, tmp_fence.nv_fences[0].value, -1);

    if (R_SUCCEEDED(rc)) g_gfxCurrentBuffer = (g_gfxCurrentBuffer + 1) & (g_nvgfx_totalframebufs-1);

    //if (R_SUCCEEDED(rc)) rc = nvgfxSubmitGpfifo();

    return rc;
}

static Result _gfxQueueBuffer(s32 buf) {
    Result rc=0;

    if (buf == -1) return 0;

    g_gfxQueueBufferData.timestamp = svcGetSystemTick();//This is probably not the proper value for the timestamp, but shouldn't(?) matter.
    //if (g_nvgfx_nvhostgpu_gpfifo_fence.id) memcpy(&g_gfxQueueBufferData.fence.nv_fences[0], &g_nvgfx_nvhostgpu_gpfifo_fence, sizeof(nvioctl_fence));
    //if (g_nvgfx_nvhostgpu_gpfifo_fence.id) rc = nvgfxEventWait(g_nvgfx_nvhostgpu_gpfifo_fence.id, g_nvgfx_nvhostgpu_gpfifo_fence.value, -1);
    if (R_FAILED(rc)) return rc;

    rc = bufferProducerQueueBuffer(buf, &g_gfxQueueBufferData, &g_gfx_QueueBuffer_QueueBufferOutput);
    if (R_FAILED(rc)) return rc;

    return rc;
}

static Result _gfxInit(ViServiceType servicetype, const char *DisplayName, u32 LayerFlags, u64 LayerId, nvServiceType nv_servicetype, size_t nv_transfermem_size) {
    Result rc=0;
    u32 i=0;

    if(g_gfxInitialized)return 0;

    g_gfxNativeWindow_ID = 0;
    g_gfxDisplayVsyncEvent = INVALID_HANDLE;
    g_gfxCurrentBuffer = -1;
    g_gfxCurrentProducerBuffer = -1;
    g_gfx_ProducerConnected = 0;
    g_gfxFramebuf = NULL;
    g_gfxFramebufSize = 0;
    g_gfxDoubleBuf = 1;

    memset(g_gfx_ProducerSlotsRequested, 0, sizeof(g_gfx_ProducerSlotsRequested));
    memset(&g_gfx_DequeueBuffer_fence, 0, sizeof(g_gfx_DequeueBuffer_fence));

    if (g_gfx_framebuf_width==0 || g_gfx_framebuf_height==0) {
        g_gfx_framebuf_width = 1280;
        g_gfx_framebuf_height = 720;
    }

    g_gfx_framebuf_display_width = g_gfx_framebuf_width;
    g_gfx_framebuf_display_height = g_gfx_framebuf_height;

    g_gfx_framebuf_aligned_width = (g_gfx_framebuf_width+15) & ~15;//Align to 16.
    g_gfx_framebuf_aligned_height = (g_gfx_framebuf_height+127) & ~127;//Align to 128.

    g_gfx_singleframebuf_size = g_gfx_framebuf_aligned_width*g_gfx_framebuf_aligned_height*4;

    g_gfx_BufferInitData.width = g_gfx_framebuf_width;
    g_gfx_BufferInitData.height = g_gfx_framebuf_height;
    g_gfx_BufferInitData.stride = g_gfx_framebuf_aligned_width;

    g_gfx_BufferInitData.data.width_unk0 = g_gfx_framebuf_width;
    g_gfx_BufferInitData.data.width_unk1 = g_gfx_framebuf_width;
    g_gfx_BufferInitData.data.height_unk = g_gfx_framebuf_height;

    g_gfx_BufferInitData.data.byte_stride = g_gfx_framebuf_aligned_width*4;

    g_gfx_BufferInitData.data.buffer_size0 = g_gfx_singleframebuf_size;
    g_gfx_BufferInitData.data.buffer_size1 = g_gfx_singleframebuf_size;

    rc = viInitialize(servicetype);
    if (R_FAILED(rc)) return rc;

    rc = viOpenDisplay(DisplayName, &g_gfxDisplay);

    if (R_SUCCEEDED(rc)) rc = viGetDisplayVsyncEvent(&g_gfxDisplay, &g_gfxDisplayVsyncEvent);

    if (R_SUCCEEDED(rc)) rc = viOpenLayer(g_gfxNativeWindow, &g_gfxNativeWindow_Size, &g_gfxDisplay, &g_gfxLayer, LayerFlags, LayerId);

    if (R_SUCCEEDED(rc)) rc = viSetLayerScalingMode(&g_gfxLayer, ViScalingMode_Default);

    if (R_SUCCEEDED(rc)) rc = _gfxGetNativeWindowID(g_gfxNativeWindow, g_gfxNativeWindow_Size, &g_gfxNativeWindow_ID);

    if (R_SUCCEEDED(rc)) {
        binderCreateSession(&g_gfxBinderSession, viGetSession_IHOSBinderDriverRelay(), g_gfxNativeWindow_ID);
        rc = binderInitSession(&g_gfxBinderSession, 0x0f);
    }

    if (R_SUCCEEDED(rc)) rc = nvInitialize(nv_servicetype, nv_transfermem_size);

    if (R_SUCCEEDED(rc)) rc = bufferProducerInitialize(&g_gfxBinderSession);

    if (R_SUCCEEDED(rc)) rc = bufferProducerConnect(NATIVE_WINDOW_API_CPU, 0, &g_gfx_Connect_QueueBufferOutput);

    if (R_SUCCEEDED(rc)) g_gfx_ProducerConnected = 1;

    if (R_SUCCEEDED(rc)) rc = nvgfxInitialize();

    if (R_SUCCEEDED(rc)) rc = nvgfxGetFramebuffer(&g_gfxFramebuf, &g_gfxFramebufSize);

    if (R_SUCCEEDED(rc)) { //Official sw would use bufferProducerRequestBuffer() when required during swap-buffers/or similar, but that's not really an option here due to gfxSetDoubleBuffering().
       for(i=0; i<2; i++) {
           rc = _gfxDequeueBuffer();
           if (R_FAILED(rc)) break;

           rc = bufferProducerRequestBuffer(g_gfxCurrentProducerBuffer, NULL);
           if (R_FAILED(rc)) break;

           g_gfx_ProducerSlotsRequested[i] = 1;

           //Officially, nvioctlNvmap_FromID() and nvioctlChannel_SubmitGPFIFO() are used here.

           rc = _gfxQueueBuffer(g_gfxCurrentProducerBuffer);
           if (R_FAILED(rc)) {
               g_gfxCurrentProducerBuffer = -1;
               break;
           }
       }
    }

    if (R_SUCCEEDED(rc)) rc = _gfxDequeueBuffer();

    if (R_SUCCEEDED(rc)) {
        if (__nx_applet_type == AppletType_Application) { //It's unknown whether there's a better way to handle this.
            svcSleepThread(2000000000);
        }
        else {
            for(i=0; i<2; i++) gfxWaitForVsync();
        }
    }

    if (R_FAILED(rc)) {
        _gfxQueueBuffer(g_gfxCurrentProducerBuffer);
        for(i=0; i<2; i++) {
            if (g_gfx_ProducerSlotsRequested[i]) bufferProducerDetachBuffer(i);
        }
        if (g_gfx_ProducerConnected) bufferProducerDisconnect(NATIVE_WINDOW_API_CPU);

        nvgfxExit();
        bufferProducerExit();
        binderExitSession(&g_gfxBinderSession);
        nvExit();

        viCloseLayer(&g_gfxLayer);
        viCloseDisplay(&g_gfxDisplay);
        viExit();

        if(g_gfxDisplayVsyncEvent != INVALID_HANDLE) {
            svcCloseHandle(g_gfxDisplayVsyncEvent);
            g_gfxDisplayVsyncEvent = INVALID_HANDLE;
        }

        g_gfxNativeWindow_ID = 0;
        g_gfxCurrentBuffer = 0;
        g_gfxCurrentProducerBuffer = -1;
        g_gfx_ProducerConnected = 0;
        g_gfxFramebuf = NULL;
        g_gfxFramebufSize = 0;

        g_gfx_framebuf_width = 0;
        g_gfx_framebuf_height = 0;

        memset(g_gfx_ProducerSlotsRequested, 0, sizeof(g_gfx_ProducerSlotsRequested));
    }

    if (R_SUCCEEDED(rc)) g_gfxInitialized = 1;

    return rc;
}

void gfxInitDefault(void)
{
    nvServiceType nv_servicetype;

    switch(__nx_applet_type) {
    case AppletType_Application:
    case AppletType_SystemApplication:
        nv_servicetype = NVSERVTYPE_Application;
        break;

    case AppletType_SystemApplet:
    case AppletType_LibraryApplet:
    case AppletType_OverlayApplet:
    default:
        nv_servicetype = NVSERVTYPE_Applet;
        break;
    }

    Result rc = _gfxInit(ViLayerFlags_Default, "Default", ViLayerFlags_Default, 0, nv_servicetype, 0x300000);
    if (R_FAILED(rc)) fatalSimple(rc);
}

void gfxExit(void)
{
    u32 i = 0;
    if (!g_gfxInitialized)
        return;

    _gfxQueueBuffer(g_gfxCurrentProducerBuffer);
    for (i=0; i<2; i++) {
        if (g_gfx_ProducerSlotsRequested[i]) bufferProducerDetachBuffer(i);
    }
    if (g_gfx_ProducerConnected) bufferProducerDisconnect(2);

    nvgfxExit();

    bufferProducerExit();
    binderExitSession(&g_gfxBinderSession);

    nvExit();

    viCloseLayer(&g_gfxLayer);

    if(g_gfxDisplayVsyncEvent != INVALID_HANDLE) {
        svcCloseHandle(g_gfxDisplayVsyncEvent);
        g_gfxDisplayVsyncEvent = INVALID_HANDLE;
    }

    viCloseDisplay(&g_gfxDisplay);

    viExit();

    g_gfxInitialized = 0;
    g_gfxNativeWindow_ID = 0;

    g_gfxCurrentBuffer = 0;
    g_gfxCurrentProducerBuffer = -1;
    g_gfx_ProducerConnected = 0;
    g_gfxFramebuf = NULL;
    g_gfxFramebufSize = 0;

    g_gfx_framebuf_width = 0;
    g_gfx_framebuf_height = 0;

    gfxConfigureAutoResolution(0, 0, 0, 0, 0);

    memset(g_gfx_ProducerSlotsRequested, 0, sizeof(g_gfx_ProducerSlotsRequested));

    memset(&g_gfxQueueBufferData.crop, 0, sizeof(g_gfxQueueBufferData.crop));
}

void gfxInitResolution(u32 width, u32 height) {
    if (g_gfxInitialized) fatalSimple(MAKERESULT(MODULE_LIBNX, LIBNX_ALREADYINITIALIZED));

    g_gfx_framebuf_width = width;
    g_gfx_framebuf_height = height;
}

void gfxInitResolutionDefault(void) {
    gfxInitResolution(1920, 1080);
}

void gfxConfigureCrop(s32 left, s32 top, s32 right, s32 bottom) {
    if (right==0 || bottom==0) {
        g_gfx_framebuf_display_width = g_gfx_framebuf_width;
        g_gfx_framebuf_display_height = g_gfx_framebuf_height;
    }

    if (left < 0 || top < 0 || right < 0 || bottom < 0) return;
    if (right < left || bottom < top) return;
    if (left > g_gfx_framebuf_width || top > g_gfx_framebuf_height) return;
    if (right > g_gfx_framebuf_width || bottom > g_gfx_framebuf_height) return;

    g_gfxQueueBufferData.crop.left = left;
    g_gfxQueueBufferData.crop.top = top;
    g_gfxQueueBufferData.crop.right = right;
    g_gfxQueueBufferData.crop.bottom = bottom;

    if (right!=0 && bottom!=0) {
        g_gfx_framebuf_display_width = right;
        g_gfx_framebuf_display_height = bottom;
    }
}

void gfxConfigureResolution(s32 width, s32 height) {
    gfxConfigureCrop(0, 0, width, height);
}

static void _gfxAutoResolutionAppletHook(AppletHookType hook, void* param) {
    u8 mode=0;

    if (hook != AppletHookType_OnOperationMode)
        return;

    mode = appletGetOperationMode();

    if (mode == AppletOperationMode_Handheld) {
        gfxConfigureResolution(g_gfx_autoresolution_handheld_width, g_gfx_autoresolution_handheld_height);
    }
    else if (mode == AppletOperationMode_Docked) {
        gfxConfigureResolution(g_gfx_autoresolution_docked_width, g_gfx_autoresolution_docked_height);
    }
}

void gfxConfigureAutoResolution(bool enable, s32 handheld_width, s32 handheld_height, s32 docked_width, s32 docked_height) {
    if (g_gfx_autoresolution_enabled != enable) {
        if(enable) appletHook(&g_gfx_autoresolution_applethookcookie, _gfxAutoResolutionAppletHook, 0);
        if (!enable) appletUnhook(&g_gfx_autoresolution_applethookcookie);
    }

    g_gfx_autoresolution_enabled = enable;

    g_gfx_autoresolution_handheld_width = handheld_width;
    g_gfx_autoresolution_handheld_height = handheld_height;
    g_gfx_autoresolution_docked_width = docked_width;
    g_gfx_autoresolution_docked_height = docked_height;

    if (enable) _gfxAutoResolutionAppletHook(AppletHookType_OnOperationMode, 0);
    if (!enable) gfxConfigureResolution(0, 0);
}

void gfxConfigureAutoResolutionDefault(bool enable) {
    gfxConfigureAutoResolution(enable, 1280, 720, 0, 0);
}

Result _gfxGraphicBufferInit(s32 buf, u32 nvmap_handle) {
    g_gfx_BufferInitData.refcount = buf;
    g_gfx_BufferInitData.data.nvmap_handle0 = nvmap_handle;
    g_gfx_BufferInitData.data.nvmap_handle1 = nvmap_handle;
    g_gfx_BufferInitData.data.buffer_offset = g_gfx_singleframebuf_size*buf;
    g_gfx_BufferInitData.data.timestamp = svcGetSystemTick();

    return bufferProducerGraphicBufferInit(buf, &g_gfx_BufferInitData);
}

static void _waitevent(Handle *handle) {
    s32 tmpindex=0;
    Result rc=0, rc2=0;

    svcResetSignal(*handle);

    do {
        rc = svcWaitSynchronization(&tmpindex, handle, 1, U64_MAX);
        if (R_SUCCEEDED(rc)) rc2 = svcResetSignal(*handle);
    } while(R_FAILED(rc) || (rc2 & 0x3FFFFF)==0xFA01);

    if (R_FAILED(rc2)) fatalSimple(rc2);
}

void gfxWaitForVsync(void) {
    _waitevent(&g_gfxDisplayVsyncEvent);
}

void gfxSwapBuffers(void) {
    Result rc=0;

    rc = _gfxQueueBuffer(g_gfxCurrentProducerBuffer);

    if (R_SUCCEEDED(rc)) rc = _gfxDequeueBuffer();

    if (R_FAILED(rc)) fatalSimple(rc);
}

u8* gfxGetFramebuffer(u32* width, u32* height) {
    if(width) *width = g_gfx_framebuf_display_width;
    if(height) *height = g_gfx_framebuf_display_height;

    return &g_gfxFramebuf[g_gfxCurrentBuffer*g_gfx_singleframebuf_size];
}

void gfxGetFramebufferResolution(u32* width, u32* height) {
    if(width) *width = g_gfx_framebuf_width;
    if(height) *height = g_gfx_framebuf_height;
}

size_t gfxGetFramebufferSize(void) {
    return g_gfx_singleframebuf_size;
}

void gfxSetDoubleBuffering(bool doubleBuffering) {
    g_gfxDoubleBuf = doubleBuffering;
}

void gfxFlushBuffers(void) {
    armDCacheFlush(&g_gfxFramebuf[g_gfxCurrentBuffer*g_gfx_singleframebuf_size], g_gfx_singleframebuf_size);
}

/*static Result _gfxGetDisplayResolution(u64 *width, u64 *height) {
    return viGetDisplayResolution(&g_gfxDisplay, width, height);
}*/

