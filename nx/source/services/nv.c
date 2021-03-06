#include <string.h>
#include <switch.h>

static Handle g_nvServiceSession = INVALID_HANDLE;
static size_t g_nvIpcBufferSize = 0;
static u32 g_nvServiceType = -1;
static TransferMemory g_nvTransfermem;

static Result _nvInitialize(Handle proc, Handle sharedmem, u32 transfermem_size);
static Result _nvSetClientPID(u64 AppletResourceUserId);

Result nvInitialize(nvServiceType servicetype, size_t transfermem_size) {
    if(g_nvServiceType!=-1)return MAKERESULT(MODULE_LIBNX, LIBNX_ALREADYINITIALIZED);

    Result rc = 0;
    u64 AppletResourceUserId = 0;

    if (servicetype==NVSERVTYPE_Default || servicetype==NVSERVTYPE_Application) {
        rc = smGetService(&g_nvServiceSession, "nvdrv");
        g_nvServiceType = 0;
    }

    if ((servicetype==NVSERVTYPE_Default && R_FAILED(rc)) || servicetype==NVSERVTYPE_Applet) {
        rc = smGetService(&g_nvServiceSession, "nvdrv:a");
        g_nvServiceType = 1;
    }

    if ((servicetype==NVSERVTYPE_Default && R_FAILED(rc)) || servicetype==NVSERVTYPE_Sysmodule)
    {
        rc = smGetService(&g_nvServiceSession, "nvdrv:s");
        g_nvServiceType = 2;
    }

    if ((servicetype==NVSERVTYPE_Default && R_FAILED(rc)) || servicetype==NVSERVTYPE_T)
    {
        rc = smGetService(&g_nvServiceSession, "nvdrv:t");
        g_nvServiceType = 3;
    }

    if (R_SUCCEEDED(rc)) {
        g_nvIpcBufferSize = 0;
        rc = ipcQueryPointerBufferSize(g_nvServiceSession, &g_nvIpcBufferSize);

        if (R_SUCCEEDED(rc)) rc = tmemCreate(&g_nvTransfermem, transfermem_size, PERM_NONE);

        if (R_SUCCEEDED(rc)) rc = _nvInitialize(CUR_PROCESS_HANDLE, g_nvTransfermem.handle, transfermem_size);

        //Officially ipc control DuplicateSessionEx would be used here.

        if (R_SUCCEEDED(rc)) rc = appletGetAppletResourceUserId(&AppletResourceUserId);//TODO: How do sysmodules handle this?

        if (R_SUCCEEDED(rc)) rc = _nvSetClientPID(AppletResourceUserId);
    }

    if (R_FAILED(rc)) {
        g_nvServiceType = -1;

        if(g_nvServiceSession != INVALID_HANDLE)
        {
            svcCloseHandle(g_nvServiceSession);
            g_nvServiceSession = INVALID_HANDLE;
        }

        tmemClose(&g_nvTransfermem);
    }

    return rc;
}

void nvExit(void)
{
    if(g_nvServiceType==-1)return;

    g_nvServiceType = -1;

    if(g_nvServiceSession != INVALID_HANDLE)
    {
        svcCloseHandle(g_nvServiceSession);
        g_nvServiceSession = INVALID_HANDLE;
    }

    tmemClose(&g_nvTransfermem);
}

static Result _nvInitialize(Handle proc, Handle sharedmem, u32 transfermem_size) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
        u32 transfermem_size;
    } *raw;

    ipcSendHandleCopy(&c, proc);
    ipcSendHandleCopy(&c, sharedmem);

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 3;
    raw->transfermem_size = transfermem_size;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
        } *resp = r.Raw;

        rc = resp->result;
    }

    return rc;
}

static Result _nvSetClientPID(u64 AppletResourceUserId) {
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
    raw->cmd_id = 8;
    raw->AppletResourceUserId = AppletResourceUserId;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
        } *resp = r.Raw;

        rc = resp->result;
    }

    return rc;
}

Result nvOpen(u32 *fd, const char *devicepath) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
    } *raw;

    ipcAddSendBuffer(&c, devicepath, strlen(devicepath), 0);

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 0;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
            u32 fd;
            u32 error;
        } *resp = r.Raw;

        rc = resp->result;
        if (R_SUCCEEDED(rc)) rc = resp->error;
        if (R_SUCCEEDED(rc)) *fd = resp->fd;
    }

    return rc;
}

Result nvIoctl(u32 fd, u32 request, void* argp) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
        u32 fd;
        u32 request;
    } *raw;

    size_t bufsize = _IOC_SIZE(request);
    u32 dir = _IOC_DIR(request);

    void* buf_send = NULL, *buf_recv = NULL;
    size_t buf_send_size = 0, buf_recv_size = 0;

    if(dir & _IOC_WRITE) {
        buf_send = argp;
        buf_send_size = bufsize;
    }

    if(dir & _IOC_READ) {
        buf_recv = argp;
        buf_recv_size = bufsize;
    }

    void* bufs_send[2] = {buf_send, buf_send};
    void* bufs_recv[2] = {buf_recv, buf_recv};
    size_t bufs_send_size[2] = {buf_send_size, buf_send_size};
    size_t bufs_recv_size[2] = {buf_recv_size, buf_recv_size};

    if(g_nvIpcBufferSize!=0 && bufsize <= g_nvIpcBufferSize) {
        bufs_send[0] = NULL;
        bufs_send_size[0] = 0;
        bufs_recv[0] = NULL;
        bufs_recv_size[0] = 0;
    }
    else {
        bufs_send[1] = NULL;
        bufs_send_size[1] = 0;
        bufs_recv[1] = NULL;
        bufs_recv_size[1] = 0;
    }

    ipcAddSendBuffer(&c, bufs_send[0], bufs_send_size[0], 0);
    ipcAddRecvBuffer(&c, bufs_recv[0], bufs_recv_size[0], 0);

    ipcAddSendStatic(&c, bufs_send[1], bufs_send_size[1], 0);
    ipcAddRecvStatic(&c, bufs_recv[1], bufs_recv_size[1], 0);

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 1;
    raw->fd = fd;
    raw->request = request;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
            u32 error;
        } *resp = r.Raw;

        rc = resp->result;
        if (R_SUCCEEDED(rc)) rc = resp->error;
    }

    return rc;
}

Result nvClose(u32 fd) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
        u32 fd;
    } *raw;

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 2;
    raw->fd = fd;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
            u32 error;
        } *resp = r.Raw;

        rc = resp->result;
        if (R_SUCCEEDED(rc)) rc = resp->error;
    }

    return rc;
}

Result nvQueryEvent(u32 fd, u32 event_id, Handle *handle_out) {
    IpcCommand c;
    ipcInitialize(&c);

    struct {
        u64 magic;
        u64 cmd_id;
        u32 fd;
        u32 event_id;
    } *raw;

    raw = ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 4;
    raw->fd = fd;
    raw->event_id = event_id;

    Result rc = ipcDispatch(g_nvServiceSession);

    if (R_SUCCEEDED(rc)) {
        IpcParsedCommand r;
        ipcParse(&r);

        struct {
            u64 magic;
            u64 result;
            u32 error;
        } *resp = r.Raw;

        rc = resp->result;
        if (R_SUCCEEDED(rc)) rc = resp->error;
        if (R_SUCCEEDED(rc)) *handle_out = r.Handles[0];
    }

    return rc;
}

