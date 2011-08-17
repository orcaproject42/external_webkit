/*
 * Copyright 2011, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "TransferQueue.h"

#if USE(ACCELERATED_COMPOSITING)

#include "BaseTile.h"
#include "PaintedSurface.h"
#include <android/native_window.h>
#include <gui/SurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>

#ifdef DEBUG
#include <cutils/log.h>
#include <wtf/text/CString.h>

#undef XLOG
#define XLOG(...) android_printLog(ANDROID_LOG_DEBUG, "TransferQueue", __VA_ARGS__)

#else

#undef XLOG
#define XLOG(...)

#endif // DEBUG

#define ST_BUFFER_NUMBER 4

namespace WebCore {

TransferQueue::TransferQueue()
    : m_eglSurface(EGL_NO_SURFACE)
    , m_transferQueueIndex(0)
    , m_fboID(0)
    , m_sharedSurfaceTextureId(0)
    , m_hasGLContext(true)
{
    memset(&m_GLStateBeforeBlit, 0, sizeof(m_GLStateBeforeBlit));

    m_emptyItemCount = ST_BUFFER_NUMBER;

    m_transferQueue = new TileTransferData[ST_BUFFER_NUMBER];
    for (int i = 0; i < ST_BUFFER_NUMBER; i++) {
        m_transferQueue[i].savedBaseTilePtr = 0;
        m_transferQueue[i].status = emptyItem;
    }
}

TransferQueue::~TransferQueue()
{
    glDeleteFramebuffers(1, &m_fboID);
    m_fboID = 0;
    glDeleteTextures(1, &m_sharedSurfaceTextureId);
    m_sharedSurfaceTextureId = 0;

    delete[] m_transferQueue;
}

void TransferQueue::initSharedSurfaceTextures(int width, int height)
{
    if (!m_sharedSurfaceTextureId) {
        glGenTextures(1, &m_sharedSurfaceTextureId);
        m_sharedSurfaceTexture =
            new android::SurfaceTexture(m_sharedSurfaceTextureId);
        m_ANW = new android::SurfaceTextureClient(m_sharedSurfaceTexture);
        m_sharedSurfaceTexture->setSynchronousMode(true);
        m_sharedSurfaceTexture->setBufferCount(ST_BUFFER_NUMBER+1);

        int result = native_window_set_buffers_geometry(m_ANW.get(),
                width, height, HAL_PIXEL_FORMAT_RGBA_8888);
        GLUtils::checkSurfaceTextureError("native_window_set_buffers_geometry", result);
        result = native_window_set_usage(m_ANW.get(),
                GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN);
        GLUtils::checkSurfaceTextureError("native_window_set_usage", result);
    }

    if (!m_fboID)
        glGenFramebuffers(1, &m_fboID);
}

// When bliting, if the item from the transfer queue is mismatching b/t the
// BaseTile and the content, then the item is considered as obsolete, and
// the content is discarded.
bool TransferQueue::checkObsolete(int index)
{
    BaseTile* baseTilePtr = m_transferQueue[index].savedBaseTilePtr;
    if (!baseTilePtr) {
        XLOG("Invalid savedBaseTilePtr , such that the tile is obsolete");
        return true;
    }

    BaseTileTexture* baseTileTexture = baseTilePtr->texture();
    if (!baseTileTexture) {
        XLOG("Invalid baseTileTexture , such that the tile is obsolete");
        return true;
    }

    const TextureTileInfo* tileInfo = &m_transferQueue[index].tileInfo;

    if (tileInfo->m_x != baseTilePtr->x()
        || tileInfo->m_y != baseTilePtr->y()
        || tileInfo->m_scale != baseTilePtr->scale()
        || tileInfo->m_painter != baseTilePtr->painter()) {
        XLOG("Mismatching x, y, scale or painter , such that the tile is obsolete");
        return true;
    }

    return false;
}

void TransferQueue::blitTileFromQueue(GLuint fboID, BaseTileTexture* destTex, GLuint srcTexId, GLenum srcTexTarget)
{
    // Then set up the FBO and copy the SurfTex content in.
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           destTex->m_ownTextureId,
                           0);
    setGLStateForCopy(destTex->getSize().width(),
                      destTex->getSize().height());
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        XLOG("Error: glCheckFramebufferStatus failed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    // Use empty rect to set up the special matrix to draw.
    SkRect rect  = SkRect::MakeEmpty();
    TilesManager::instance()->shader()->drawQuad(rect, srcTexId, 1.0,
                       srcTexTarget);

    // Clean up FBO setup.
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // rebind the standard FBO

    // Add a sync point here to WAR a driver bug.
    glViewport(0, 0, 0, 0);
    TilesManager::instance()->shader()->drawQuad(rect, destTex->m_ownTextureId,
                                                 1.0, GL_TEXTURE_2D);

    GLUtils::checkGlError("copy the surface texture into the normal one");
}

void TransferQueue::interruptTransferQueue(bool interrupt)
{
    m_transferQueueItemLocks.lock();
    m_interruptedByRemovingOp = interrupt;
    if (m_interruptedByRemovingOp)
        m_transferQueueItemCond.signal();
    m_transferQueueItemLocks.unlock();
}

// This function must be called inside the m_transferQueueItemLocks, for the
// wait, m_interruptedByRemovingOp and getHasGLContext().
// Only called by updateQueueWithBitmap() for now.
bool TransferQueue::readyForUpdate()
{
    if (!getHasGLContext())
        return false;
    // Don't use a while loop since when the WebView tear down, the emptyCount
    // will still be 0, and we bailed out b/c of GL context lost.
    if (!m_emptyItemCount) {
        if (m_interruptedByRemovingOp)
            return false;
        m_transferQueueItemCond.wait(m_transferQueueItemLocks);
        if (m_interruptedByRemovingOp)
            return false;
    }

    if (!getHasGLContext())
        return false;

    return true;
}

// Both getHasGLContext and setHasGLContext should be called within the lock.
bool TransferQueue::getHasGLContext()
{
    return m_hasGLContext;
}

void TransferQueue::setHasGLContext(bool hasContext)
{
    m_hasGLContext = hasContext;
}

// Only called when WebView is destroyed.
void TransferQueue::discardQueue()
{
    android::Mutex::Autolock lock(m_transferQueueItemLocks);

    for (int i = 0 ; i < ST_BUFFER_NUMBER; i++)
        m_transferQueue[i].status = pendingDiscard;

    bool GLContextExisted = getHasGLContext();
    // Unblock the Tex Gen thread first before Tile Page deletion.
    // Otherwise, there will be a deadlock while removing operations.
    setHasGLContext(false);

    // Only signal once when GL context lost.
    if (GLContextExisted)
        m_transferQueueItemCond.signal();
}

// Call on UI thread to copy from the shared Surface Texture to the BaseTile's texture.
void TransferQueue::updateDirtyBaseTiles()
{
    saveGLState();
    android::Mutex::Autolock lock(m_transferQueueItemLocks);

    cleanupTransportQueue();
    if (!getHasGLContext())
        setHasGLContext(true);

    // Start from the oldest item, we call the updateTexImage to retrive
    // the texture and blit that into each BaseTile's texture.
    const int nextItemIndex = getNextTransferQueueIndex();
    int index = nextItemIndex;
    for (int k = 0; k < ST_BUFFER_NUMBER ; k++) {
        if (m_transferQueue[index].status == pendingBlit) {
            bool obsoleteBaseTile = checkObsolete(index);
            // Save the needed info, update the Surf Tex, clean up the item in
            // the queue. Then either move on to next item or copy the content.
            BaseTileTexture* destTexture = 0;
            if (!obsoleteBaseTile)
                destTexture = m_transferQueue[index].savedBaseTilePtr->texture();

            m_sharedSurfaceTexture->updateTexImage();

            m_transferQueue[index].savedBaseTilePtr = 0;
            m_transferQueue[index].status = emptyItem;
            if (obsoleteBaseTile) {
                XLOG("Warning: the texture is obsolete for this baseTile");
                index = (index + 1) % ST_BUFFER_NUMBER;
                continue;
            }

            blitTileFromQueue(m_fboID, destTexture,
                              m_sharedSurfaceTextureId,
                              m_sharedSurfaceTexture->getCurrentTextureTarget());

            // After the base tile copied into the GL texture, we need to
            // update the texture's info such that at draw time, readyFor
            // will find the latest texture's info
            // We don't need a map any more, each texture contains its own
            // texturesTileInfo.
            destTexture->setOwnTextureTileInfoFromQueue(&m_transferQueue[index].tileInfo);

            XLOG("Blit tile x, y %d %d to destTexture->m_ownTextureId %d",
                 m_transferQueue[index].tileInfo.m_x,
                 m_transferQueue[index].tileInfo.m_y,
                 destTexture->m_ownTextureId);
        }
        index = (index + 1) % ST_BUFFER_NUMBER;
    }

    restoreGLState();

    m_emptyItemCount = ST_BUFFER_NUMBER;
    m_transferQueueItemCond.signal();
}

void TransferQueue::updateQueueWithBitmap(const TileRenderInfo* renderInfo,
                                          int x, int y, const SkBitmap& bitmap)
{
    android::Mutex::Autolock lock(m_transferQueueItemLocks);

    bool ready = readyForUpdate();

    if (!ready) {
        XLOG("Quit bitmap update: not ready! for tile x y %d %d",
             renderInfo->x, renderInfo->y);
        return;
    }

    // a) Dequeue the Surface Texture and write into the buffer
    if (!m_ANW.get()) {
        XLOG("ERROR: ANW is null");
        return;
    }

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(m_ANW.get(), &buffer, 0))
        return;

    uint8_t* img = (uint8_t*)buffer.bits;
    int row, col;
    int bpp = 4; // Now we only deal with RGBA8888 format.
    int width = TilesManager::instance()->tileWidth();
    int height = TilesManager::instance()->tileHeight();
    if (!x && !y && bitmap.width() == width && bitmap.height() == height) {
        bitmap.lockPixels();
        uint8_t* bitmapOrigin = static_cast<uint8_t*>(bitmap.getPixels());
        // Copied line by line since we need to handle the offsets and stride.
        for (row = 0 ; row < bitmap.height(); row ++) {
            uint8_t* dst = &(img[(buffer.stride * (row + x) + y) * bpp]);
            uint8_t* src = &(bitmapOrigin[bitmap.width() * row * bpp]);
            memcpy(dst, src, bpp * bitmap.width());
        }
        bitmap.unlockPixels();
    } else {
        // TODO: implement the partial invalidate here!
        XLOG("ERROR: don't expect to get here yet before we support partial inval");
    }

    ANativeWindow_unlockAndPost(m_ANW.get());

    // b) After update the Surface Texture, now udpate the transfer queue info.
    addItemInTransferQueue(renderInfo);
    XLOG("Bitmap updated x, y %d %d, baseTile %p",
         renderInfo->x, renderInfo->y, renderInfo->baseTile);
}

// Note that there should be lock/unlock around this function call.
// Currently only called by GLUtils::updateSharedSurfaceTextureWithBitmap.
void TransferQueue::addItemInTransferQueue(const TileRenderInfo* renderInfo)
{
    m_transferQueueIndex = (m_transferQueueIndex + 1) % ST_BUFFER_NUMBER;

    int index = m_transferQueueIndex;
    if (m_transferQueue[index].savedBaseTilePtr
        || m_transferQueue[index].status != emptyItem) {
        XLOG("ERROR update a tile which is dirty already @ index %d", index);
    }

    m_transferQueue[index].savedBaseTilePtr = renderInfo->baseTile;
    m_transferQueue[index].status = pendingBlit;

    // Now fill the tileInfo.
    TextureTileInfo* textureInfo = &m_transferQueue[index].tileInfo;

    textureInfo->m_x = renderInfo->x;
    textureInfo->m_y = renderInfo->y;
    textureInfo->m_scale = renderInfo->scale;
    textureInfo->m_painter = renderInfo->tilePainter;

    textureInfo->m_picture = renderInfo->textureInfo->m_pictureCount;

    m_emptyItemCount--;
}

// Note: this need to be called within th lock.
// Only called by updateDirtyBaseTiles() for now
void TransferQueue::cleanupTransportQueue()
{
    int index = getNextTransferQueueIndex();

    for (int i = 0 ; i < ST_BUFFER_NUMBER; i++) {
        if (m_transferQueue[index].status == pendingDiscard) {
            m_sharedSurfaceTexture->updateTexImage();

            m_transferQueue[index].savedBaseTilePtr = 0;
            m_transferQueue[index].status = emptyItem;
        }
        index = (index + 1) % ST_BUFFER_NUMBER;
    }
}

void TransferQueue::saveGLState()
{
    glGetIntegerv(GL_VIEWPORT, m_GLStateBeforeBlit.viewport);
    glGetBooleanv(GL_SCISSOR_TEST, m_GLStateBeforeBlit.scissor);
    glGetBooleanv(GL_DEPTH_TEST, m_GLStateBeforeBlit.depth);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, m_GLStateBeforeBlit.clearColor);
}

void TransferQueue::setGLStateForCopy(int width, int height)
{
    // Need to match the texture size.
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

void TransferQueue::restoreGLState()
{
    glViewport(m_GLStateBeforeBlit.viewport[0],
               m_GLStateBeforeBlit.viewport[1],
               m_GLStateBeforeBlit.viewport[2],
               m_GLStateBeforeBlit.viewport[3]);

    if (m_GLStateBeforeBlit.scissor[0])
        glEnable(GL_SCISSOR_TEST);

    if (m_GLStateBeforeBlit.depth)
        glEnable(GL_DEPTH_TEST);

    glClearColor(m_GLStateBeforeBlit.clearColor[0],
                 m_GLStateBeforeBlit.clearColor[1],
                 m_GLStateBeforeBlit.clearColor[2],
                 m_GLStateBeforeBlit.clearColor[3]);
}

int TransferQueue::getNextTransferQueueIndex()
{
    return (m_transferQueueIndex + 1) % ST_BUFFER_NUMBER;
}

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING
