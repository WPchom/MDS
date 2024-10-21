/**
 * Copyright (c) [2022] [pchom]
 * [MDS_FS] is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 **/
/* Include ----------------------------------------------------------------- */
#include "mds_fs.h"

/* Variable ---------------------------------------------------------------- */
static MDS_Mutex_t g_fsLock;
static MDS_ListNode_t g_fsList = {.prev = &g_fsList, .next = &g_fsList};
static const char *g_fsCwdPath = "/";

/* Memory ------------------------------------------------------------------ */
__attribute__((weak)) void *MDS_FsMalloc(size_t size)
{
    return (MDS_SysMemAlloc(size));
}

__attribute__((weak)) void MDS_FsFree(void *ptr)
{
    return (MDS_SysMemFree(ptr));
}

__attribute__((weak)) void *MDS_FsCalloc(size_t numb, size_t size)
{
    return (MDS_SysMemCalloc(numb, size));
}

__attribute__((weak)) char *MDS_FsStrdup(const char *str)
{
    size_t len = strlen(str) + 1;
    char *dup = MDS_FsMalloc(len);
    if (dup != NULL) {
        MDS_MemBuffCopy(dup, len, str, len);
    }

    return (dup);
}

/* Path -------------------------------------------------------------------- */
static char *MDS_FileSystemNormalizePath(char *fullpath)
{
    char *src = fullpath;
    char *dst = fullpath;

    while (*src != '\0') {
        if (src[0] == '.') {
            if (src[1] == '/') {  // "./"
                for (src += 1; *src == '/'; src++) {
                }
                continue;
            } else if ((src[1] == '.') && (src[1 + 1] == '/')) {  // "../"
                for (src += 1 + 1; *src == '/'; src++) {
                }
                if (dst <= fullpath) {
                    return (NULL);
                }
                do {
                    dst--;
                } while ((fullpath < dst) && (dst[-1] != '/'));
            }
        }

        while ((*src != '\0') && (*src != '/')) {
            *dst++ = *src++;
        }
        if (*src == '/') {
            for (*dst++ = '/'; *src == '/'; src++) {
            }
        }
    }

    if (dst > fullpath) {
        dst[0] = '\0';
    }

    return (fullpath);
}

char *MDS_FileSystemJoinPath(const char *path, ...)
{
    va_list args;
    size_t len = 0;
    const char *s = path;

    va_start(args, path);
    for (const char *p = path; p != NULL; p = va_arg(args, const char *)) {
        if (p[0] == '/') {
            s = p;
            len = strlen(p) + 1;
        } else {
            len += strlen(p) + 1;
        }
    }
    va_end(args);

    char *fullpath = (len > 0) ? (MDS_FsMalloc(len)) : (NULL);
    if (fullpath != NULL) {
        va_start(args, path);
        for (const char *p = path; p != s; p = va_arg(args, const char *)) {
        }
        size_t ofs = 0;
        while (s != NULL) {
            if (ofs != 0) {
                fullpath[ofs++] = '/';
            }
            for (size_t slen = strlen(s); slen != 0; slen--) {
                fullpath[ofs++] = *s++;
            }
            s = va_arg(args, const char *);
        }
        fullpath[ofs] = '\0';
        va_end(args);
    }

    return (MDS_FileSystemNormalizePath(fullpath));
}

void MDS_FileSystemFreePath(char *path)
{
    MDS_FsFree(path);
}

/* FileSystem -------------------------------------------------------------- */
static const MDS_FileSystemOps_t *MDS_FileSystemFindOps(const char *fsName)
{
    static const void *mdsFsOpsBegin __attribute__((section(MDS_FILE_SYSTEM_SECTION "\000"))) = NULL;
    static const void *mdsFsOpsLimit __attribute__((section(MDS_FILE_SYSTEM_SECTION "\177"))) = NULL;
    const MDS_FileSystemOps_t *mdsFsOpsS = (const MDS_FileSystemOps_t *)((uintptr_t)(&mdsFsOpsBegin) + sizeof(void *));
    const MDS_FileSystemOps_t *mdsFsOPsE = (const MDS_FileSystemOps_t *)((uintptr_t)(&mdsFsOpsLimit));

    for (const MDS_FileSystemOps_t *ops = mdsFsOpsS; ops < mdsFsOPsE; ops++) {
        if (strcmp(ops->name, fsName) == 0) {
            return (ops);
        }
    }

    return (NULL);
}

static void MDS_FsLock(void)
{
    MDS_Err_t err;

    if (MDS_ObjectGetType(&(g_fsLock.object)) != MDS_OBJECT_TYPE_MUTEX) {
        err = MDS_MutexInit(&g_fsLock, "fs");
    }

    do {
        err = MDS_MutexAcquire(&g_fsLock, MDS_TICK_FOREVER);
    } while (err != MDS_EOK);
}

static void MDS_FsUnlock(void)
{
    MDS_MutexRelease(&g_fsLock);
}

static void MDS_FileSystemInsert(MDS_FileSystem_t *fs)
{
    MDS_FileSystem_t *find = NULL;
    MDS_FileSystem_t *iter = NULL;

    MDS_LIST_FOREACH_NEXT (iter, fsNode, &g_fsList) {
        if (strcmp(fs->path, iter->path) < 0) {
            find = iter;
            break;
        }
    }

    if (find == NULL) {
        MDS_ListInsertNodePrev(&g_fsList, &(fs->fsNode));
    } else {
        MDS_ListInsertNodePrev(&(find->fsNode), &(fs->fsNode));
    }
}

static MDS_FileSystem_t *MDS_FileSystemLookup(const char *abspath)
{
    MDS_FileSystem_t *iter = NULL;

    MDS_LIST_FOREACH_NEXT (iter, fsNode, &g_fsList) {
        if (strncmp(abspath, iter->path, strlen(iter->path)) == 0) {
            return (iter);
        }
    }

    return (NULL);
}

static const MDS_FileSystem_t *MDS_FileSystemFindMountDevice(MDS_FsDevice_t *device)
{
    MDS_FileSystem_t *iter = NULL;

    MDS_LIST_FOREACH_NEXT (iter, fsNode, &g_fsList) {
        if (iter->device != device) {
            continue;
        }
        return (iter);
    }

    return (NULL);
}

static const MDS_FileSystem_t *MDS_FileSystemFindMountPath(const char *abspath)
{
    size_t fplen = strlen(abspath);
    const MDS_FileSystem_t *iter = NULL;

    MDS_LIST_FOREACH_NEXT (iter, fsNode, &g_fsList) {
        size_t plen = strlen(iter->path);
        if (strncmp(abspath, iter->path, (fplen > plen) ? (plen) : (fplen))) {
            continue;
        }
        return (iter);
    }

    return (NULL);
}

void MDS_FileSystemChangeCwd(const char *path)
{
    g_fsCwdPath = path;
}

MDS_Err_t MDS_FileSystemMkfs(MDS_FsDevice_t *device, const char *fsName)
{
    MDS_ASSERT(device != NULL);
    MDS_ASSERT(fsName != NULL);

    MDS_Err_t err = MDS_EOK;

    MDS_FsLock();
    const MDS_FileSystem_t *fs = MDS_FileSystemFindMountDevice(device);
    if (fs != NULL) {
        err = MDS_EBUSY;
    }
    MDS_FsUnlock();

    if (err == MDS_EOK) {
        const MDS_FileSystemOps_t *ops = MDS_FileSystemFindOps(fsName);
        if (ops == NULL) {
            err = MDS_EIO;
        } else if (ops->mkfs != NULL) {
            err = ops->mkfs(device);
        }
    }

    return (err);
}

MDS_Err_t MDS_FileSystemMount(const MDS_FsDevice_t *device, const char *path, const char *fsName)
{
    MDS_ASSERT(device != NULL);
    MDS_ASSERT(path != NULL);
    MDS_ASSERT(fsName != NULL);

    MDS_Err_t err = MDS_EOK;

    const MDS_FileSystemOps_t *ops = MDS_FileSystemFindOps(fsName);
    if (ops == NULL) {
        return (MDS_EIO);
    }

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, "./", NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    do {
        if (MDS_FileSystemFindMountPath(abspath) != NULL) {
            err = MDS_EEXIST;
            break;
        }

        MDS_FileSystem_t *fs = MDS_FsCalloc(1, sizeof(MDS_FileSystem_t));
        if (fs == NULL) {
            err = MDS_ENOMEM;
            break;
        }

        MDS_MutexInit(&(fs->lock), "fs");
        MDS_ListInitNode(&(fs->fsNode));
        MDS_ListInitNode(&(fs->fileList));
        fs->path = abspath;
        fs->device = device;
        fs->ops = ops;

        if (ops->mount != NULL) {
            err = ops->mount(fs);
        }

        if (err == MDS_EOK) {
            MDS_FileSystemInsert(fs);
        } else {
            MDS_FsFree(fs);
        }
    } while (0);
    MDS_FsUnlock();

    if (err != MDS_EOK) {
        MDS_FileSystemFreePath(abspath);
    }

    return (err);
}

MDS_Err_t MDS_FileSystemUnmount(const char *path)
{
    MDS_ASSERT(path != NULL);

    MDS_Err_t err = MDS_EOK;

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, "./", NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    do {
        MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
        if (fs == NULL) {
            err = MDS_ENOENT;
            break;
        }

        MDS_MutexAcquire(&(fs->lock), MDS_TICK_FOREVER);
        if (!MDS_ListIsEmpty(&(fs->fileList))) {
            MDS_MutexRelease(&(fs->lock));
            err = MDS_EBUSY;
            break;
        }

        if ((fs->ops != NULL) && (fs->ops->unmount != NULL)) {
            err = fs->ops->unmount(fs);
        }

        if (err != MDS_EOK) {
            MDS_MutexRelease(&(fs->lock));
            break;
        }

        MDS_ListRemoveNode(&(fs->fsNode));
        MDS_MutexDeInit(&(fs->lock));
        MDS_FileSystemFreePath(fs->path);
        MDS_FsFree(fs);
    } while (0);
    MDS_FsUnlock();

    MDS_FileSystemFreePath(abspath);

    return (err);
}

MDS_Err_t MDS_FileSystemStatfs(const char *path, MDS_FsStat_t *statfs)
{
    MDS_Err_t err = MDS_EIO;

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, "./", NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
    MDS_FsUnlock();

    if ((fs != NULL) && (fs->ops != NULL) && (fs->ops->statfs != NULL)) {
        err = fs->ops->statfs(fs, statfs);
    }

    MDS_FileSystemFreePath(abspath);

    return (err);
}

/* File -------------------------------------------------------------------- */
static MDS_FileNode_t *MDS_FileNodeLookup(MDS_FileSystem_t *fs, const char *path)
{
    MDS_FileNode_t *iter = NULL;
    MDS_LIST_FOREACH_NEXT (iter, fileNode, &(fs->fileList)) {
        if (strcmp(iter->path, path) == 0) {
            return (iter);
        }
    }

    return (NULL);
}

static MDS_FileNode_t *MDS_FileNodeCreate(MDS_FileSystem_t *fs, const char *abspath)
{
    MDS_FileNode_t *fnode = MDS_FsCalloc(1, sizeof(MDS_FileNode_t));
    if (fnode != NULL) {
        fnode->path = MDS_FsStrdup(&(abspath[strlen(fs->path)]));
        if (fnode->path == NULL) {
            MDS_FsFree(fnode);
            fnode = NULL;
        } else {
            MDS_ListInitNode(&(fnode->fileNode));
            MDS_ListInsertNodePrev(&(fs->fileList), &(fnode->fileNode));
            fnode->refCount = 1;
            fnode->fs = fs;
        }
    }

    return (fnode);
}

MDS_Err_t MDS_FileOpen(MDS_FileDesc_t *fd, const char *path, MDS_Mask_t flags)
{
    MDS_ASSERT(fd != NULL);
    MDS_ASSERT(path != NULL);

    MDS_Err_t err = MDS_EOK;

    if (fd->node != NULL) {
        return (MDS_EEXIST);
    }

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, NULL);
    if (abspath == NULL) {
        return (MDS_EINVAL);
    }

    MDS_FsLock();
    MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
    MDS_FsUnlock();
    if ((fs == NULL) || (fs->ops == NULL) || (fs->ops->open == NULL)) {
        MDS_FileSystemFreePath(abspath);
        return (MDS_ENOENT);
    }

    MDS_MutexAcquire(&(fs->lock), MDS_TICK_FOREVER);
    do {
        MDS_FileNode_t *fnode = MDS_FileNodeLookup(fs, abspath);
        if (fnode != NULL) {
            fnode->refCount += 1;
        } else {
            fnode = MDS_FileNodeCreate(fs, abspath);
            if (fnode == NULL) {
                err = MDS_ENOMEM;
                break;
            }
        }

        fd->node = fnode;
        fd->pos = 0;
        fd->flags = flags | MDS_FFLAG_OPEN;

        err = fs->ops->open(fd);
        if (err != MDS_EOK) {
            fnode->refCount -= 1;
            if (fnode->refCount <= 1) {
                MDS_ListRemoveNode(&(fnode->fileNode));
                MDS_FsFree(fnode->path);
                MDS_FsFree(fnode);
            }
            fd->node = NULL;
            fd->pos = 0;
            fd->flags = MDS_FFLAG_NONE;
        }
    } while (0);
    MDS_MutexRelease(&(fs->lock));

    MDS_FileSystemFreePath(abspath);

    return (err);
}

MDS_Err_t MDS_FileClose(MDS_FileDesc_t *fd)
{
    MDS_ASSERT(fd != NULL);
    MDS_ASSERT(fd->node != NULL);
    MDS_ASSERT(fd->node->fs != NULL);

    MDS_Err_t err = MDS_EOK;
    MDS_FileNode_t *fnode = fd->node;
    MDS_FileSystem_t *fs = fnode->fs;

    MDS_MutexAcquire(&(fs->lock), MDS_TICK_FOREVER);
    do {
        if ((fs->ops != NULL) && (fs->ops->close != NULL)) {
            err = fs->ops->close(fd);
        }
        if (err != MDS_EOK) {
            break;
        }

        fnode->refCount -= 1;
        if (fnode->refCount <= 0) {
            MDS_LOG_E("[fs][FileClose] fnode refCount %d <= 0", fnode->refCount);
        }
        if (fnode->refCount <= 1) {
            MDS_ListRemoveNode(&(fnode->fileNode));
            MDS_FsFree(fnode->path);
            MDS_FsFree(fnode);
        }
        fd->node = NULL;
        fd->pos = 0;
        fd->flags = MDS_FFLAG_NONE;
    } while (0);
    MDS_MutexRelease(&(fs->lock));

    return (err);
}

MDS_Err_t MDS_FileIoctl(MDS_FileDesc_t *fd, MDS_Item_t cmd, MDS_Arg_t *args)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->ioctl == NULL)) {
        return (MDS_EIO);
    }

    return (fd->node->fs->ops->ioctl(fd, cmd, args));
}

MDS_FileOffset_t MDS_FileRead(MDS_FileDesc_t *fd, uint8_t *buff, MDS_FileSize_t len)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->read == NULL)) {
        return (MDS_EIO);
    }

    MDS_Err_t err = fd->node->fs->ops->read(fd, buff, len);

    return (err);
}

MDS_FileOffset_t MDS_FileWrite(MDS_FileDesc_t *fd, const uint8_t *buff, MDS_FileSize_t len)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->write == NULL)) {
        return (MDS_EIO);
    }

    return (fd->node->fs->ops->write(fd, buff, len));
}

MDS_Err_t MDS_FileFlush(MDS_FileDesc_t *fd)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->flush == NULL)) {
        return (MDS_EIO);
    }

    MDS_Err_t err = fd->node->fs->ops->flush(fd);

    return (err);
}

MDS_FileOffset_t MDS_FileLseek(MDS_FileDesc_t *fd, MDS_FileOffset_t offset)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->lseek == NULL)) {
        return (MDS_EIO);
    }

    MDS_FileOffset_t pos = fd->node->fs->ops->lseek(fd, offset);
    if (pos >= 0) {
        fd->pos = pos;
    }

    return (pos);
}

MDS_Err_t MDS_FileFtruncate(MDS_FileDesc_t *fd, MDS_FileOffset_t len)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->ftruncate == NULL)) {
        return (MDS_EIO);
    }

    MDS_Err_t err = fd->node->fs->ops->ftruncate(fd, len);

    return (err);
}

MDS_Err_t MDS_FileGetdents(MDS_FileDesc_t *fd, MDS_Dirent_t *dirp, size_t nbytes)
{
    MDS_ASSERT(fd != NULL);

    if ((fd->node == NULL) || ((fd->flags & MDS_FFLAG_OPEN) == 0U)) {
        return (MDS_EACCES);
    }

    if ((fd->node->fs == NULL) || (fd->node->fs->ops == NULL) || (fd->node->fs->ops->getdents == NULL)) {
        return (MDS_EIO);
    }

    return (fd->node->fs->ops->getdents(fd, dirp, nbytes));
}

MDS_Err_t MDS_FileUnlink(const char *path)
{
    MDS_ASSERT(path != NULL);

    MDS_Err_t err = MDS_EOK;

    char *abspath = MDS_FileSystemJoinPath("/", path, NULL);
    if (abspath == NULL) {
        return (MDS_EINVAL);
    }

    do {
        MDS_FsLock();
        MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
        MDS_FsUnlock();
        if (fs == NULL) {
            err = MDS_ENOENT;
            break;
        }

        if ((fs->ops == NULL) || (fs->ops->unlink == NULL)) {
            err = MDS_EIO;
            break;
        }

        const char *fspath = &(abspath[strlen(fs->path)]);
        MDS_MutexAcquire(&(fs->lock), MDS_TICK_FOREVER);
        MDS_FileNode_t *fnode = MDS_FileNodeLookup(fs, fspath);
        MDS_MutexRelease(&(fs->lock));
        if (fnode != NULL) {
            err = MDS_EBUSY;
            break;
        }

        err = fs->ops->unlink(fs, fspath);
    } while (0);

    MDS_FileSystemFreePath(abspath);

    return (err);
}

MDS_Err_t MDS_FileRename(const char *oldpath, const char *newpath)
{
    MDS_Err_t err = MDS_EOK;
}

MDS_Err_t MDS_FileMkdir(const char *path)
{
    MDS_Err_t err = MDS_EIO;

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, "./", NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
    MDS_FsUnlock();

    if ((fs != NULL) && (fs->ops != NULL) && (fs->ops->open != NULL)) {
        MDS_FileDesc_t fd = {0};
        fd.flags |= MDS_OFLAG_DIRECOTY;
        err = fs->ops->open(&fd);
    }
}

MDS_Err_t MDS_FileRemove(const char *path)
{
    MDS_ASSERT(path != NULL);

    MDS_Err_t err = MDS_EPERM;

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
    MDS_FsUnlock();

    if ((fs != NULL) && (fs->ops != NULL) && (fs->ops->remove != NULL)) {
        err = fs->ops->remove(fs, &(abspath[strlen(fs->path)]));
    }

    MDS_FileSystemFreePath(abspath);

    return (err);
}

MDS_Err_t MDS_FileRename(const char *oldpath, const char *newpath)
{
    MDS_ASSERT(oldpath != NULL);
    MDS_ASSERT(newpath != NULL);

    MDS_Err_t err = MDS_EPERM;
    char *oldabspath = NULL;
    char *newabspath = NULL;

    do {
        oldabspath = MDS_FileSystemJoinPath(g_fsCwdPath, oldpath, NULL);
        if (oldabspath == NULL) {
            err = MDS_ENOMEM;
            break;
        }

        newabspath = MDS_FileSystemJoinPath(g_fsCwdPath, newpath, NULL);
        if (newabspath == NULL) {
            err = MDS_ENOMEM;
            break;
        }

        MDS_FsLock();
        MDS_FileSystem_t *oldfs = MDS_FileSystemLookup(oldabspath);
        MDS_FileSystem_t *newfs = MDS_FileSystemLookup(newabspath);
        MDS_FsUnlock();
        if (oldfs != newfs) {
            // copy
            break;
        }

        const char *oldfspath = &(oldabspath[strlen(oldfs->path)]);
        const char *newfspath = &(newabspath[strlen(newfs->path)]);
        MDS_MutexAcquire(&(oldfs->lock), MDS_TICK_FOREVER);
        MDS_FileNode_t *fnode = MDS_FileNodeLookup(oldfs, oldfspath);
        MDS_MutexRelease(&(oldfs->lock));
        if (fnode != NULL) {
            err = MDS_EBUSY;
            break;
        }

        if ((oldfs != NULL) && (oldfs->ops != NULL) && (oldfs->ops->rename != NULL)) {
            err = oldfs->ops->rename(oldfs, oldfspath, newfspath);
        }
    } while (0);

    if (oldabspath != NULL) {
        MDS_FileSystemFreePath(oldabspath);
    }
    if (newabspath != NULL) {
        MDS_FileSystemFreePath(newabspath);
    }

    return (err);
}

MDS_Err_t MDS_FileLink(const char *srcpath, const char *dstpath)
{
}

MDS_Err_t MDS_FileStat(const char *path, MDS_FileStat_t *stat)
{
    MDS_ASSERT(path != NULL);

    MDS_Err_t err = MDS_EPERM;

    char *abspath = MDS_FileSystemJoinPath(g_fsCwdPath, path, NULL);
    if (abspath == NULL) {
        return (MDS_ENOMEM);
    }

    MDS_FsLock();
    MDS_FileSystem_t *fs = MDS_FileSystemLookup(abspath);
    MDS_FsUnlock();

    if ((fs != NULL) && (fs->ops != NULL) && (fs->ops->stat != NULL)) {
        err = fs->ops->stat(fs, &(abspath[strlen(fs->path)]), stat);
    }

    MDS_FileSystemFreePath(abspath);

    return (err);
}
