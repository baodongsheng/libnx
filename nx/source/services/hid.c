#include <string.h>
#include <switch.h>

static Handle g_hidServiceSession = INVALID_HANDLE;
static Handle g_hidIAppletResource = INVALID_HANDLE;
static SharedMemory g_hidSharedmem;

static HidTouchScreenEntry g_touchEntry;
static HidMouseEntry g_mouseEntry;
static HidKeyboardEntry g_keyboardEntry;
static HidControllerInputEntry g_controllerEntries[10];

static u64 g_mouseOld, g_mouseHeld, g_mouseDown, g_mouseUp;
static u64 g_keyboardModOld, g_keyboardModHeld, g_keyboardModDown, g_keyboardModUp;
static u32 g_keyboardOld[8], g_keyboardHeld[8], g_keyboardDown[8], g_keyboardUp[8];
static u64 g_controllerOld[10], g_controllerHeld[10], g_controllerDown[10], g_controllerUp[10];

static HidControllerLayoutType g_controllerLayout[10];
static u64 g_touchTimestamp, g_mouseTimestamp, g_keyboardTimestamp, g_controllerTimestamps[10];

static RwLock g_hidLock;

static Result _hidCreateAppletResource(Handle sessionhandle, Handle* handle_out, u64 AppletResourceUserId);
static Result _hidGetSharedMemoryHandle(Handle sessionhandle, Handle* handle_out);

Result hidInitialize(void)
{
    if (g_hidServiceSession != INVALID_HANDLE)
        return MAKERESULT(MODULE_LIBNX, LIBNX_ALREADYINITIALIZED);

    Result rc = 0;
    u64 AppletResourceUserId = 0;
    Handle sharedmem_handle=0;

    rc = appletGetAppletResourceUserId(&AppletResourceUserId);
    if (R_FAILED(rc))
        return rc;

    rc = smGetService(&g_hidServiceSession, "hid");
    if (R_FAILED(rc))
        return rc;

    rc = _hidCreateAppletResource(g_hidServiceSession, &g_hidIAppletResource, AppletResourceUserId);

    if (R_SUCCEEDED(rc))
        rc = _hidGetSharedMemoryHandle(g_hidIAppletResource, &sharedmem_handle);

    if (R_SUCCEEDED(rc)) {
        shmemLoadRemote(&g_hidSharedmem, sharedmem_handle, 0x40000, PERM_R);

        rc = shmemMap(&g_hidSharedmem);

        if (R_FAILED(rc)) svcCloseHandle(sharedmem_handle);
    }

    if (R_FAILED(rc)) {
        if (g_hidServiceSession != INVALID_HANDLE)
            svcCloseHandle(g_hidServiceSession);

        if (g_hidIAppletResource != INVALID_HANDLE)
            svcCloseHandle(g_hidIAppletResource);

        g_hidServiceSession = INVALID_HANDLE;
        g_hidIAppletResource = INVALID_HANDLE;
    }

    hidReset();

    return rc;
}

void hidExit(void)
{
    if (g_hidServiceSession == INVALID_HANDLE)
        return;

    if (g_hidServiceSession != INVALID_HANDLE) {
        svcCloseHandle(g_hidServiceSession);
        g_hidServiceSession = INVALID_HANDLE;
    }

    if (g_hidIAppletResource != INVALID_HANDLE) {
        svcCloseHandle(g_hidIAppletResource);
        g_hidIAppletResource = INVALID_HANDLE;
    }

    shmemClose(&g_hidSharedmem);
}

void hidReset(void)
{
    rwlockWriteLock(&g_hidLock);

    // Reset internal state
    memset(&g_touchEntry, 0, sizeof(HidTouchScreenEntry));
    memset(&g_mouseEntry, 0, sizeof(HidMouseEntry));
    memset(&g_keyboardEntry, 0, sizeof(HidKeyboardEntry));
    memset(g_controllerEntries, 0, sizeof(g_controllerEntries));

    g_mouseOld = g_mouseHeld = g_mouseDown = g_mouseUp = 0;
    g_keyboardModOld = g_keyboardModHeld = g_keyboardModDown = g_keyboardModUp = 0;
    for (int i = 0; i < 8; i++)
        g_keyboardOld[i] = g_keyboardHeld[i] = g_keyboardDown[i] = g_keyboardUp[i] = 0;
    for (int i = 0; i < 10; i++)
        g_controllerOld[i] = g_controllerHeld[i] = g_controllerDown[i] = g_controllerUp[i] = 0;

    for (int i = 0; i < 10; i++)
        g_controllerLayout[i] = LAYOUT_DEFAULT;

    g_touchTimestamp = g_mouseTimestamp = g_keyboardTimestamp = 0;
    for (int i = 0; i < 10; i++)
        g_controllerTimestamps[i] = 0;

    rwlockWriteUnlock(&g_hidLock);
}

Handle hidGetSessionService(void) {
    return g_hidServiceSession;
}

void* hidGetSharedmemAddr(void) {
    return shmemGetAddr(&g_hidSharedmem);
}

void hidSetControllerLayout(HidControllerID id, HidControllerLayoutType layoutType) {
    rwlockWriteLock(&g_hidLock);
    g_controllerLayout[id] = layoutType;
    rwlockWriteUnlock(&g_hidLock);
}

HidControllerLayoutType hidGetControllerLayout(HidControllerID id) {
    rwlockReadLock(&g_hidLock);
    HidControllerLayoutType tmp = g_controllerLayout[id];
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

void hidScanInput(void) {
    if (g_hidServiceSession == INVALID_HANDLE)
        return;

    rwlockWriteLock(&g_hidLock);

    HidSharedMemory *sharedMem = (HidSharedMemory*)hidGetSharedmemAddr();

    g_mouseOld = g_mouseHeld;
    g_keyboardModOld = g_keyboardModHeld;
    memcpy(g_keyboardOld, g_keyboardHeld, sizeof(g_keyboardOld));
    memcpy(g_controllerOld, g_controllerHeld, sizeof(g_controllerOld));

    g_mouseHeld = 0;
    g_keyboardModHeld = 0;
    memset(g_keyboardHeld, 0, sizeof(g_keyboardHeld));
    memset(g_controllerHeld, 0, sizeof(g_controllerHeld));
    memset(&g_touchEntry, 0, sizeof(HidTouchScreenEntry));
    memset(&g_mouseEntry, 0, sizeof(HidMouseEntry));
    memset(&g_keyboardEntry, 0, sizeof(HidKeyboardEntry));
    memset(g_controllerEntries, 0, sizeof(g_controllerEntries));

    u64 latestTouchEntry = sharedMem->touchscreen.header.latestEntry;
    HidTouchScreenEntry *newTouchEntry = &sharedMem->touchscreen.entries[latestTouchEntry];
    if ((s64)(newTouchEntry->header.timestamp - g_touchTimestamp) > 0) {
        memcpy(&g_touchEntry, newTouchEntry, sizeof(HidTouchScreenEntry));
        g_touchTimestamp = newTouchEntry->header.timestamp;

        if (hidTouchCount())
            g_controllerHeld[CONTROLLER_HANDHELD] |= KEY_TOUCH;
    }

    u64 latestMouseEntry = sharedMem->mouse.header.latestEntry;
    HidMouseEntry *newMouseEntry = &sharedMem->mouse.entries[latestMouseEntry];
    if ((s64)(newMouseEntry->timestamp - g_mouseTimestamp) > 0) {
        memcpy(&g_mouseEntry, newMouseEntry, sizeof(HidMouseEntry));
        g_mouseTimestamp = newMouseEntry->timestamp;

        g_mouseHeld = g_mouseEntry.buttons;
    }
    g_mouseDown = (~g_mouseOld) & g_mouseHeld;
    g_mouseUp = g_mouseOld & (~g_mouseHeld);

    u64 latestKeyboardEntry = sharedMem->keyboard.header.latestEntry;
    HidKeyboardEntry *newKeyboardEntry = &sharedMem->keyboard.entries[latestKeyboardEntry];
    if ((s64)(newKeyboardEntry->timestamp - g_keyboardTimestamp) > 0) {
        memcpy(&g_keyboardEntry, newKeyboardEntry, sizeof(HidKeyboardEntry));
        g_keyboardTimestamp = newKeyboardEntry->timestamp;

        g_keyboardModHeld = g_keyboardEntry.modifier;
        for (int i = 0; i < 8; i++) {
            g_keyboardHeld[i] = g_keyboardEntry.keys[i];
        }
    }
    g_keyboardModDown = (~g_keyboardModOld) & g_keyboardModHeld;
    g_keyboardModUp = g_keyboardModOld & (~g_keyboardModHeld);
    for (int i = 0; i < 8; i++) {
        g_keyboardDown[i] = (~g_keyboardOld[i]) & g_keyboardHeld[i];
        g_keyboardUp[i] = g_keyboardOld[i] & (~g_keyboardHeld[i]);
    }

    for (int i = 0; i < 10; i++) {
        HidControllerLayout *currentLayout = &sharedMem->controllers[i].layouts[g_controllerLayout[i]];
        u64 latestControllerEntry = currentLayout->header.latestEntry;
        HidControllerInputEntry *newInputEntry = &currentLayout->entries[latestControllerEntry];
        if ((s64)(newInputEntry->timestamp - g_controllerTimestamps[i]) > 0) {
            memcpy(&g_controllerEntries[i], newInputEntry, sizeof(HidControllerInputEntry));
            g_controllerTimestamps[i] = newInputEntry->timestamp;

            g_controllerHeld[i] |= g_controllerEntries[i].buttons;
        }

        g_controllerDown[i] = (~g_controllerOld[i]) & g_controllerHeld[i];
        g_controllerUp[i] = g_controllerOld[i] & (~g_controllerHeld[i]);
    }

    rwlockWriteUnlock(&g_hidLock);
}

u64 hidKeysHeld(HidControllerID id) {
    if (id < 0 || id > 9) return 0;

    rwlockReadLock(&g_hidLock);
    u64 tmp = g_controllerHeld[id];
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

u64 hidKeysDown(HidControllerID id) {
    if (id < 0 || id > 9) return 0;

    rwlockReadLock(&g_hidLock);
    u64 tmp = g_controllerDown[id];
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

u64 hidKeysUp(HidControllerID id) {
    if (id < 0 || id > 9) return 0;

    rwlockReadLock(&g_hidLock);
    u64 tmp = g_controllerUp[id];
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

u64 hidMouseButtonsHeld(void) {
    rwlockReadLock(&g_hidLock);
    u64 tmp = g_mouseHeld;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

u64 hidMouseButtonsDown(void) {
    rwlockReadLock(&g_hidLock);
    u64 tmp = g_mouseDown;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

u64 hidMouseButtonsUp(void) {
    rwlockReadLock(&g_hidLock);
    u64 tmp = g_mouseUp;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

bool hidKeyboardModifierHeld(HidKeyboardModifier modifier) {
    rwlockReadLock(&g_hidLock);
    bool tmp = g_keyboardModHeld & modifier;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

bool hidKeyboardModifierDown(HidKeyboardModifier modifier) {
    rwlockReadLock(&g_hidLock);
    bool tmp = g_keyboardModDown & modifier;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

bool hidKeyboardModifierUp(HidKeyboardModifier modifier) {
    rwlockReadLock(&g_hidLock);
    bool tmp = g_keyboardModUp & modifier;
    rwlockReadUnlock(&g_hidLock);

    return tmp;
}

bool hidKeyboardHeld(HidKeyboardScancode key) {
    rwlockReadLock(&g_hidLock);
    u32 tmp = g_keyboardHeld[key / 32] & (1 << (key % 32));
    rwlockReadUnlock(&g_hidLock);

    return !!tmp;
}

bool hidKeyboardDown(HidKeyboardScancode key) {
    rwlockReadLock(&g_hidLock);
    u32 tmp = g_keyboardDown[key / 32] & (1 << (key % 32));
    rwlockReadUnlock(&g_hidLock);

    return !!tmp;
}

bool hidKeyboardUp(HidKeyboardScancode key) {
    rwlockReadLock(&g_hidLock);
    u32 tmp = g_keyboardUp[key / 32] & (1 << (key % 32));
    rwlockReadUnlock(&g_hidLock);

    return !!tmp;
}

u32 hidTouchCount(void) {
    return g_touchEntry.header.numTouches;
}

void hidTouchRead(touchPosition *pos, u32 point_id) {
    if (pos) {
        if (point_id >= g_touchEntry.header.numTouches) {
            memset(pos, 0, sizeof(touchPosition));
            return;
        }

        pos->px = g_touchEntry.touches[point_id].x;
        pos->py = g_touchEntry.touches[point_id].y;
        pos->dx = g_touchEntry.touches[point_id].diameterX;
        pos->dy = g_touchEntry.touches[point_id].diameterY;
        pos->angle = g_touchEntry.touches[point_id].angle;
    }
}

static Result _hidCreateAppletResource(Handle sessionhandle, Handle* handle_out, u64 AppletResourceUserId) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
        u64 AppletResourceUserId;
    } *raw;

    ipcSendPid(&c);

    raw = ipcPrepareHeader(&c, sizeof(*raw));

    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 0;
    raw->AppletResourceUserId = AppletResourceUserId;

    Result rc = ipcDispatch(sessionhandle);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
        } *resp = r.Raw;

        rc = resp->result;

        if (R_SUCCEEDED(rc)) {
            *handle_out = r.Handles[0];
        }
    }

    return rc;
}

static Result _hidGetSharedMemoryHandle(Handle sessionhandle, Handle* handle_out) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
    } *raw;

    raw = ipcPrepareHeader(&c, sizeof(*raw));

    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 0;

    Result rc = ipcDispatch(sessionhandle);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
        } *resp = r.Raw;

        rc = resp->result;

        if (R_SUCCEEDED(rc)) {
            *handle_out = r.Handles[0];
        }
    }

    return rc;
}

