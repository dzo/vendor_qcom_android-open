/* ------------------------------------------------------------------
 * Copyright (C) 2009 Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "VideoMio72xx"
#include <utils/Log.h>

#include "android_surface_output_msm72xx.h"
#include <media/PVPlayer.h>

#include <cutils/properties.h>

#define PLATFORM_PRIVATE_PMEM 1

#if HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif

#include <gralloc_priv.h>

using namespace android;

static const char* pmem_adsp = "/dev/pmem_adsp";
static const char* pmem = "/dev/pmem";

OSCL_EXPORT_REF AndroidSurfaceOutputMsm72xx::AndroidSurfaceOutputMsm72xx() :
    AndroidSurfaceOutput()
{
    mHardwareCodec = false;

    //Statistics profiling
    char value[PROPERTY_VALUE_MAX];
    mStatistics = false;
    mLastFrame = 0;
    mLastFpsTime = 0;
    mFpsSum = 0;
    iFrameNumber = 0;
    mNumFpsSamples = 0;
    property_get("persist.debug.pv.statistics", value, "0");
    if(atoi(value)) mStatistics = true;
}

OSCL_EXPORT_REF AndroidSurfaceOutputMsm72xx::~AndroidSurfaceOutputMsm72xx()
{
    if(mStatistics) AverageFPSPrint();
}

// create a frame buffer for software codecs
OSCL_EXPORT_REF bool AndroidSurfaceOutputMsm72xx::initCheck()
{

    // initialize only when we have all the required parameters
    if (((iVideoParameterFlags & VIDEO_SUBFORMAT_VALID) == 0) || !checkVideoParameterFlags())
        return mInitialized;

    // release resources if previously initialized
    closeFrameBuf();

    // reset flags in case display format changes in the middle of a stream
    resetVideoParameterFlags();

    // copy parameters in case we need to adjust them
    int displayWidth = iVideoDisplayWidth;
    int displayHeight = iVideoDisplayHeight;
    int frameWidth = iVideoWidth;
    int frameHeight = iVideoHeight;
    int frameSize;

    // MSM72xx hardware codec uses semi-planar format
    if ((iVideoSubFormat == PVMF_MIME_YUV420_SEMIPLANAR_YVU) ||
        (iVideoSubFormat == PVMF_MIME_YUV420_SEMIPLANAR_YVU_INTERLACE)) {
        LOGV("using hardware codec");
        mHardwareCodec = true;
        mNumberOfFramesToHold = 2;
    } else {
        LOGV("using software codec");

        // YUV420 frames are 1.5 bytes/pixel
        frameSize = (frameWidth * frameHeight * 3) / 2;

        // create frame buffer heap
        sp<MemoryHeapBase> master = new MemoryHeapBase(pmem_adsp, frameSize * kBufferCount, MemoryHeapBase::NO_CACHING);
        if (master->heapID() < 0) {
            LOGE("Error creating frame buffer heap");
            return false;
        }
        master->setDevice(pmem);
        sp<MemoryHeapPmem> heap = new MemoryHeapPmem(master, 0);
        heap->slap();
        mBufferHeap = ISurface::BufferHeap(displayWidth, displayHeight, 
                frameWidth, frameHeight, HAL_PIXEL_FORMAT_YCrCb_420_SP, heap);
        master.clear();
        mSurface->registerBuffers(mBufferHeap);

        // create frame buffers
        for (int i = 0; i < kBufferCount; i++) {
            mFrameBuffers[i] = i * frameSize;
        }

        LOGV("video = %d x %d", displayWidth, displayHeight);
        LOGV("frame = %d x %d", frameWidth, frameHeight);
        LOGV("frame #bytes = %d", frameSize);

        // register frame buffers with SurfaceFlinger
        mFrameBufferIndex = 0;
    }

    mInitialized = true;
    LOGV("sendEvent(MEDIA_SET_VIDEO_SIZE, %d, %d)", iVideoDisplayWidth, iVideoDisplayHeight);
    mPvPlayer->sendEvent(MEDIA_SET_VIDEO_SIZE, iVideoDisplayWidth, iVideoDisplayHeight);
    return mInitialized;
}

PVMFStatus AndroidSurfaceOutputMsm72xx::writeFrameBuf(uint8* aData, uint32 aDataLen, const PvmiMediaXferHeader& data_header_info)
{
    // OK to drop frames if no surface
    if (mSurface == 0) return PVMFSuccess;

    // hardware codec
    if (mHardwareCodec) {

        // initialize frame buffer heap
        if (mBufferHeap.heap == 0) {
            LOGV("initializing for hardware");
            LOGV("private data pointer is 0%p\n", data_header_info.private_data_ptr);

            // check for correct video format
            if ((iVideoSubFormat != PVMF_MIME_YUV420_SEMIPLANAR_YVU) &&
                (iVideoSubFormat != PVMF_MIME_YUV420_SEMIPLANAR_YVU_INTERLACE))
                    return PVMFFailure;

            uint32 fd;
            if (!getPmemFd(data_header_info.private_data_ptr, &fd)) {
                LOGE("Error getting pmem heap from private_data_ptr");
                return PVMFFailure;
            }

            // ugly hack to pass an sp<MemoryHeapBase> as an int
            sp<MemoryHeapBase> master = (MemoryHeapBase *) fd;
            master->setDevice(pmem);

            // create new reference
            uint32_t heap_flags = master->getFlags() & MemoryHeapBase::NO_CACHING;
            sp<MemoryHeapPmem> heap = new MemoryHeapPmem(master, heap_flags);
            heap->slap();

            if(iVideoSubFormat == PVMF_MIME_YUV420_SEMIPLANAR_YVU) {
                // register frame buffers with SurfaceFlinger
                LOGV("creating buffers for PVMF_MIME_YUV420_SEMIPLANAR_YVU");
                mBufferHeap = ISurface::BufferHeap(iVideoDisplayWidth, iVideoDisplayHeight,
                        iVideoWidth, iVideoHeight, HAL_PIXEL_FORMAT_YCrCb_420_SP, heap);
            } else {
                // register frame buffers with SurfaceFlinger
                LOGV("creating buffers for PVMF_MIME_YUV420_SEMIPLANAR_YVU_INTERLACE");
                mBufferHeap = ISurface::BufferHeap(iVideoDisplayWidth, iVideoDisplayHeight,
                        iVideoWidth, iVideoHeight, HAL_PIXEL_FORMAT_YCrCb_420_SP ^ HAL_PIXEL_FORMAT_INTERLACE, heap);
            }

            master.clear();
            mSurface->registerBuffers(mBufferHeap);
        }

        // get pmem offset and post to SurfaceFlinger
        if (!getOffset(data_header_info.private_data_ptr, &mOffset)) {
            LOGE("Error getting pmem offset from private_data_ptr");
            return PVMFFailure;
        }
        mSurface->postBuffer(mOffset);
    } else {
        // software codec
        if (++mFrameBufferIndex == kBufferCount) mFrameBufferIndex = 0;
        convertFrame(aData, static_cast<uint8*>(mBufferHeap.heap->base()) + mFrameBuffers[mFrameBufferIndex], aDataLen);
        // post to SurfaceFlinger
        mSurface->postBuffer(mFrameBuffers[mFrameBufferIndex]);
    }

    //Average FPS profiling
    if(mStatistics) AverageFPSProfiling();

    return PVMFSuccess;
}

// post the last video frame to refresh screen after pause
void AndroidSurfaceOutputMsm72xx::postLastFrame()
{
    // ignore if no surface or heap
    if ((mSurface == NULL) || (mBufferHeap.heap == NULL)) return;

    if (mHardwareCodec) {
        mSurface->postBuffer(mOffset);
    } else {
        mSurface->postBuffer(mFrameBuffers[mFrameBufferIndex]);
    }
}

bool AndroidSurfaceOutputMsm72xx::getPmemFd(OsclAny *private_data_ptr, uint32 *pmemFD)
{
    PLATFORM_PRIVATE_LIST *listPtr = NULL;
    PLATFORM_PRIVATE_PMEM_INFO *pmemInfoPtr = NULL;
    bool returnType = false;
    LOGV("in getPmemfd - privatedataptr=%p\n",private_data_ptr);
    listPtr = (PLATFORM_PRIVATE_LIST*) private_data_ptr;

    for (uint32 i=0;i<listPtr->nEntries;i++)
    {
        if(listPtr->entryList->type == PLATFORM_PRIVATE_PMEM)
        {
            LOGV("in getPmemfd - entry type = %d\n",listPtr->entryList->type);
          pmemInfoPtr = (PLATFORM_PRIVATE_PMEM_INFO*) (listPtr->entryList->entry);
          returnType = true;
          if(pmemInfoPtr){
            *pmemFD = pmemInfoPtr->pmem_fd;
            LOGV("in getPmemfd - pmemFD = %d\n",*pmemFD);
          }
          break;
        }
    }
    return returnType;
}

bool AndroidSurfaceOutputMsm72xx::getOffset(OsclAny *private_data_ptr, uint32 *offset)
{
    PLATFORM_PRIVATE_LIST *listPtr = NULL;
    PLATFORM_PRIVATE_PMEM_INFO *pmemInfoPtr = NULL;
    bool returnType = false;

    listPtr = (PLATFORM_PRIVATE_LIST*) private_data_ptr;
    LOGV("in getOffset: listPtr = %p\n",listPtr);
    for (uint32 i=0;i<listPtr->nEntries;i++)
    {
        if(listPtr->entryList->type == PLATFORM_PRIVATE_PMEM)
        {
            LOGV(" in getOffset: entrytype = %d\n",listPtr->entryList->type);

          pmemInfoPtr = (PLATFORM_PRIVATE_PMEM_INFO*) (listPtr->entryList->entry);
          returnType = true;
          if(pmemInfoPtr){
            *offset = pmemInfoPtr->offset;
            LOGV("in getOffset: offset = %d\n",*offset);
          }
          break;
        }
    }
    return returnType;
}

static inline void* byteOffset(void* p, size_t offset) { return (void*)((uint8_t*)p + offset); }

void AndroidSurfaceOutputMsm72xx::convertFrame(void* src, void* dst, size_t len)
{
    // copy the Y plane
    size_t y_plane_size = iVideoWidth * iVideoHeight;
    //LOGV("len=%u, y_plane_size=%u", len, y_plane_size);
    memcpy(dst, src, y_plane_size + iVideoWidth);

    // re-arrange U's and V's
    uint16_t* pu = (uint16_t*)byteOffset(src, y_plane_size);
    uint16_t* pv = (uint16_t*)byteOffset(pu, y_plane_size / 4);
    uint32_t* p = (uint32_t*)byteOffset(dst, y_plane_size);

    int count = y_plane_size / 8;
    //LOGV("u = %p, v = %p, p = %p, count = %d", pu, pv, p, count);
    do {
        uint32_t u = *pu++;
        uint32_t v = *pv++;
        *p++ = ((u & 0xff) << 8) | ((u & 0xff00) << 16) | (v & 0xff) | ((v & 0xff00) << 8);
    } while (--count);
}

// factory function for playerdriver linkage
extern "C" AndroidSurfaceOutputMsm72xx* createVideoMio()
{
    return new AndroidSurfaceOutputMsm72xx();
}

void AndroidSurfaceOutputMsm72xx::AverageFPSProfiling()
{
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    nsecs_t diff = now - mLastFpsTime;
    iFrameNumber++;

    if (diff > ms2ns(250)) {
        float mFps =  ((iFrameNumber - mLastFrame) * float(s2ns(1))) / diff;
        LOGE("AndroidSurfaceOutputMsm72xx: Frames Per Second: %.4f", mFps);
        mFpsSum += mFps;
        mNumFpsSamples++;
        mLastFpsTime = now;
        mLastFrame = iFrameNumber;
    }
}

void AndroidSurfaceOutputMsm72xx::AverageFPSPrint()
{
    LOGE("==========================================================");
    LOGE("AndroidSurfaceOutputMsm72xx: Average Frames Per Second: %.4f", mFpsSum / mNumFpsSamples);
    LOGE("==========================================================");
}
