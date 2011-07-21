/*
** Copyright (c) 2011 Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/*#error uncomment this for compiler test!*/

#define LOG_NDEBUG 0
#define LOG_NIDEBUG 0
#define LOG_TAG "QCameraHWI"
#include <utils/Log.h>
#include <utils/threads.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <sys/mman.h>


#include "QCameraHAL.h"
#include "QCameraHWI.h"

/* QCameraHardwareInterface class implementation goes here*/
/* following code implement the contol logic of this class*/

namespace android {

static void HAL_event_cb(mm_camera_event_t *evt, void *user_data)
{
  QCameraHardwareInterface *obj = (QCameraHardwareInterface *)user_data;
  if (obj) {
    obj->processEvent(evt);
  } else {
    LOGE("%s: NULL user_data", __func__);
  }
}

/* constructor */
QCameraHardwareInterface::
QCameraHardwareInterface(mm_camera_t *native_camera, int mode)
                  : mParameters(),
                    mPreviewHeap(0),
                    mmCamera(native_camera),
                    mPreviewRunning (false),
                    mRecordRunning (false),
                    mAutoFocusRunning(false),
                    mPreviewFrameSize(0),
                    mNotifyCb(0),
                    mDataCb(0),
                    mDataCbTimestamp(0),
                    mCallbackCookie(0),
                    mMsgEnabled(0),
                    mCurrentPreviewFrame(0), mMode(mode),
                    mStreamDisplay (NULL), mStreamRecord(NULL), mStreamSnap(NULL),
                    mVideoWidth (DEFAULT_STREAM_WIDTH),
                    mVideoHeight (DEFAULT_STREAM_HEIGHT),
                    mVideoBitrate(320000), mVideoFps (30),
                    mCameraState(CAMERA_STATE_UNINITED),
                    mZslEnable(0),
                    mDebugFps(0),
                    mInitialized(false),
                    mHasAutoFocusSupport(0),
                    mIs3DModeOn(0),
                    mPictureSizeCount(15),
                    mPreviewSizeCount(13),
                    mSupportedPictureSizesCount(15),
                    mParamStringInitialized(false),
                    mPreviewFormat(0),
                    mMaxZoom(0),
                    mZoomSupported(false),
                    mCurrentZoom(0),
                    mFps(0),
                    mPictureSizes(NULL),
                    mDumpFrmCnt(0), mDumpSkipCnt(0)
{
    LOGI("QCameraHardwareInterface: E");
    int32_t result = MM_CAMERA_E_GENERAL;
    char value[PROPERTY_VALUE_MAX];

    pthread_mutex_init(&mAsyncCmdMutex, NULL);
    pthread_cond_init(&mAsyncCmdWait, NULL);

    property_get("persist.debug.sf.showfps", value, "0");
    mDebugFps = atoi(value);

    property_get("camera.hal.fps", value, "0");
    mFps = atoi(value);

    property_get("persist.camera.hal.multitouchaf", value, "0");
    mMultiTouch = atoi(value);

    /* Open camera stack! */
    if (mmCamera) {
        result=mmCamera->ops->open(mmCamera, MM_CAMERA_OP_MODE_NOTUSED);
        if (result == MM_CAMERA_OK) {
          int i;
          mm_camera_event_type_t evt;
          for (i = 0; i < MM_CAMERA_EVT_TYPE_MAX; i++) {
            evt = (mm_camera_event_type_t) i;
            if (mmCamera->evt->is_event_supported(mmCamera, evt)){
                mmCamera->evt->register_event_notify(mmCamera,
                  HAL_event_cb, (void *)this, evt);
            }
          }
        }
    }
    LOGE("Cam open returned %d",result);
    if(MM_CAMERA_OK != result) {
          LOGE("startCamera: mm_camera_ops_open failed: handle used : 0x%p",mmCamera);
    } else {
      mCameraState = CAMERA_STATE_READY;

      /* Setup Picture Size and Preview size tables */
      setPictureSizeTable();
      LOGD("%s: Picture table size: %d", __func__, mPictureSizeCount);
      LOGD("%s: Picture table: ", __func__);
      for(int i=0; i < mPictureSizeCount;i++) {
          LOGD(" %d  %d", mPictureSizes[i].width, mPictureSizes[i].height);
      }

      setPreviewSizeTable();
      LOGD("%s: Preview table size: %d", __func__, mPreviewSizeCount);
      LOGD("%s: Preview table: ", __func__);
      for(int i=0; i < mPreviewSizeCount;i++) {
          LOGD(" %d  %d", mPreviewSizes[i].width, mPreviewSizes[i].height);
      }

      /* set my mode - update myMode member variable due to difference in
         enum definition between upper and lower layer*/
      setMyMode(mode);

      initDefaultParameters();
    }
    LOGI("QCameraHardwareInterface: X");
}


QCameraHardwareInterface::~QCameraHardwareInterface()
{
    LOGI("~QCameraHardwareInterface: E");
    int result;

    freePictureTable();

    if(mStreamDisplay){
        QCameraStream_preview::deleteInstance (mStreamDisplay);
        mStreamDisplay = NULL;
    }
    if(mStreamRecord) {
        QCameraStream_record::deleteInstance (mStreamRecord);
        mStreamRecord = NULL;
    }
    if(mStreamSnap) {
        QCameraStream_Snapshot::deleteInstance (mStreamSnap);
        mStreamSnap = NULL;
    }
    mm_camera_t* current_camera = mmCamera;
    current_camera->ops->close(current_camera);
    LOGI("~QCameraHardwareInterface: X");

}

void QCameraHardwareInterface::release()
{
    LOGI("release: E");
    Mutex::Autolock l(&mLock);

    if (isRecordingRunning()) {
        stopRecordingInternal();
        LOGI("release: stopRecordingInternal done.");
    }
    if (isPreviewRunning()) {
        stopPreviewInternal();
        LOGI("release: stopPreviewInternal done.");
    }
    if (isSnapshotRunning()) {
        cancelPictureInternal();
        LOGI("release: cancelPictureInternal done.");
    }
    if (mCameraState == CAMERA_STATE_ERROR) {
        //TBD: If Error occurs then tear down
        LOGI("release: Tear down.");
    }
    //TBD - Snapshot Teardown

    /*if(fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }*/

    LOGI("release: X");
}


sp<IMemoryHeap> QCameraHardwareInterface::getPreviewHeap() const
{
    /* TBD - Need to access buffer with an interface to relevant class
    LOGI("%s: E", __func__);
    if(mIs3DModeOn != true) {
        //TBD - Need to pass mPreviewHeap after its implemented in QCameraHWI_Preview.cpp
        return mStreamRecord->mRecordHeap != NULL ? mStreamRecord->mRecordHeap->mHeap : NULL;
    } else {
        return mStreamRecord->mRecordHeap != NULL ? mStreamRecord->mRecordHeap->mHeap : NULL;
    }*/
    return mPreviewHeap;
}

sp<IMemoryHeap> QCameraHardwareInterface::getRawHeap() const
{
    LOGI("getRawHeap: E");
    return (mStreamSnap)? mStreamSnap->getRawHeap() : NULL;
}

void QCameraHardwareInterface::useData(void* data) {

#if 0
    mm_camera_ch_data_buf_t *bufs = (mm_camera_ch_data_buf_t *)data;
    if (!bufs) {
      return;
    }

    /* yyan: depend on the channel we can choose to use data differently*/
    if (MM_CAMERA_CH_PREVIEW == bufs->type) {
      processPreviewFrame(data);
    }
    else if(MM_CAMERA_CH_VIDEO == bufs->type)
    {
      //processRecordFrame(data);
    }
#endif
    return;
}

void QCameraHardwareInterface::setCallbacks(notify_callback notify_cb,
                                      data_callback data_cb,
                                      data_callback_timestamp data_cb_timestamp,
                                      void* user)
{
    LOGE("setCallbacks: E");
    Mutex::Autolock lock(mLock);
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCallbackCookie = user;
    LOGI("setCallbacks: X");
}

void QCameraHardwareInterface::enableMsgType(int32_t msgType)
{
    LOGI("enableMsgType: E");
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
    LOGI("enableMsgType: X");
}

void QCameraHardwareInterface::disableMsgType(int32_t msgType)
{
    LOGI("disableMsgType: E");
    Mutex::Autolock lock(mLock);
    mMsgEnabled &= ~msgType;
    LOGI("disableMsgType: X");
}

bool QCameraHardwareInterface::msgTypeEnabled(int32_t msgType)
{
    LOGI("msgTypeEnabled: E");
    Mutex::Autolock lock(mLock);
    return (mMsgEnabled & msgType);
    LOGI("msgTypeEnabled: X");
}


status_t QCameraHardwareInterface::dump(int fd, const Vector<String16>& args) const
{
    LOGI("dump: E");
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    AutoMutex lock(&mLock);
    /*yan TODO: dump out diag information*/
    write(fd, result.string(), result.size());
    LOGI("dump: E");
    return NO_ERROR;
}


status_t QCameraHardwareInterface::sendCommand(int32_t command, int32_t arg1,
                                         int32_t arg2)
{
    LOGI("sendCommand: E");
    status_t rc = NO_ERROR;
    if (!mmCamera) {
        rc = BAD_VALUE;
    } else {
      Mutex::Autolock l(&mLock);

      switch (command) {
#if 0 /*TO Do: will enable it later*/
        case CAMERA_CMD_HISTOGRAM_ON:
           LOGV("histogram set to on");
           rc = setHistogramOn();
           break;
        case CAMERA_CMD_HISTOGRAM_OFF:
           LOGV("histogram set to off");
           rc = setHistogramOff();
           break;
        case CAMERA_CMD_HISTOGRAM_SEND_DATA:
          rc = NO_ERROR;
          break;
        case CAMERA_CMD_FACE_DETECTION_ON:
          if(supportsFaceDetection() == false){
              LOGI("face detection support is not available");
              rc = NO_ERROR;
          } else {
            setFaceDetection("on");
            rc = runFaceDetection();
          }
          break;

        case CAMERA_CMD_FACE_DETECTION_OFF:
          if(supportsFaceDetection() == false){
              LOGI("face detection support is not available");
              rc = NO_ERROR;
          } else {
            setFaceDetection("off");
            rc = runFaceDetection();
          }
          break;
        case CAMERA_CMD_SEND_META_DATA:
          rc = NO_ERROR;
          break;

        case CAMERA_CMD_START_SMOOTH_ZOOM :
          LOGV("HAL sendcmd start smooth zoom %d %d", arg1 , arg2);
          /*TO DO: get MaxZoom from parameter*/
          int MaxZoom = 100;

          switch(mCameraState ) {
          case CAMERA_STATE_PREVIEW:
          case CAMERA_STATE_RECORD_CMD_SENT:
          case CAMERA_STATE_RECORD:

            mTargetSmoothZoom = arg1;
            mCurrentZoom = mParameters.getInt("zoom");

            mSmoothZoomStep = (mCurrentZoom > mTargetSmoothZoom)? -1: 1;

            if(mCurrentZoom == mTargetSmoothZoom) {
                LOGV("Smoothzoom target zoom value is same as "
                     "current zoom value, return...");
                mNotifyCallback(CAMERA_MSG_ZOOM,
                    mCurrentZoom, 1, mCallbackCookie);
            } else if(mCurrentZoom < 0 || mCurrentZoom > MaxZoom ||
              mTargetSmoothZoom < 0 || mTargetSmoothZoom > MaxZoom)  {
                LOGE(" ERROR : beyond supported zoom values, break..");
                mNotifyCallback(CAMERA_MSG_ZOOM,
                    mCurrentZoom, 0, mCallbackCookie);
            } else {
              mSmoothZoomRunning = true;

              mCurrentZoom += mSmoothZoomStep;
              if ((mSmoothZoomStep < 0 && mCurrentZoom < mTargetSmoothZoom)||
                (mSmoothZoomStep > 0 && mCurrentZoom > mTargetSmoothZoom )) {
                mCurrentZoom = mTargetSmoothZoom;
              }
              mParameters.set("zoom", mCurrentZoom);
              setZoom(mParameters);
    }
            break;
          default:
            LOGV(" No preview, no smoothzoom ");
            break;
          }
          rc = NO_ERROR;
          break;

          case CAMERA_CMD_STOP_SMOOTH_ZOOM:
            if(mSmoothZoomRunning) {
              mSmoothZoomRunning = false;
              /*To Do: send cmd to stop zooming*/
            }
             LOGV("HAL sendcmd stop smooth zoom");
             rc = NO_ERROR;
             break;
#endif
          default:
            break;
      }
    }
    LOGI("sendCommand: X");
    return rc;
}

status_t QCameraHardwareInterface::getBufferInfo(sp<IMemory>& Frame, size_t *alignedSize) {
    LOGI("getBufferInfo: E");
    /*  yyan TODO: populate Frame and alignedSize. */
    status_t ret = MM_CAMERA_OK;
    if (mStreamRecord)
        mStreamRecord->getBufferInfo(Frame,alignedSize);
    LOGI("getBufferInfo: X");
    return ret;
}

void QCameraHardwareInterface::setMyMode(int mode)
{
    LOGI("setMyMode: E");
    switch(mode) {
    case CAMERA_SUPPORT_MODE_2D:
        myMode = CAMERA_MODE_2D;
        break;

    case CAMERA_SUPPORT_MODE_3D:
        myMode = CAMERA_MODE_3D;
        break;
    case CAMERA_SUPPORT_MODE_NONZSL:
        myMode = CAMERA_NONZSL_MODE;
        break;
    case CAMERA_SUPPORT_MODE_ZSL:
        myMode = CAMERA_ZSL_MODE;
        break;
    default:
        myMode = CAMERA_MODE_2D;
    }
    LOGI("setMyMode: X");
}

/* static factory function */
sp<CameraHardwareInterface> QCameraHardwareInterface::createInstance(mm_camera_t *native_camera, int mode)
{
    LOGI("createInstance: E");
    QCameraHardwareInterface *cam = new QCameraHardwareInterface(native_camera, mode);
    if (cam ) {
      if (cam->mCameraState != CAMERA_STATE_READY) {
        LOGE("createInstance: Failed");
        delete cam;
        cam = NULL;
      }
    }

    if (cam) {
      sp<CameraHardwareInterface> hardware(cam);
      LOGI("createInstance: X");
      return hardware;
    } else {
      return NULL;
    }
}

/* external plug in function */
extern "C" sp<CameraHardwareInterface>
QCameraHAL_openCameraHardware(mm_camera_t *native_camera, int mode)
{
    LOGI("QCameraHAL_openCameraHardware: E");
    return QCameraHardwareInterface::createInstance(native_camera, mode);
}

bool QCameraHardwareInterface::useOverlay(void)
{
    LOGI("useOverlay: E");
    mUseOverlay = TRUE;
    LOGI("useOverlay: X");
    return mUseOverlay;
}

status_t QCameraHardwareInterface::setOverlay(const sp<Overlay> &Overlay)
{
    LOGI("setOverlay: E");
    if( Overlay != NULL) {
        LOGE(" Valid overlay object ");
        mOverlayLock.lock();
        mOverlay = Overlay;
        mOverlayLock.unlock();
    } else {
        LOGV(" Overlay object NULL. returning ");
        mOverlayLock.lock();
        mOverlay = NULL;
        mOverlayLock.unlock();
        return UNKNOWN_ERROR;
    }
    LOGI("setOverlay: X");
    return NO_ERROR;
}

bool QCameraHardwareInterface::isPreviewRunning() {
    LOGI("isPreviewRunning: E");
    bool ret = false;
    if((mCameraState == CAMERA_STATE_PREVIEW) || (mCameraState == CAMERA_STATE_PREVIEW_START_CMD_SENT)
       || (mCameraState == CAMERA_STATE_RECORD) || (mCameraState == CAMERA_STATE_RECORD_START_CMD_SENT)) {
       return true;
    }
    LOGI("isPreviewRunning: X");
    return ret;
}

bool QCameraHardwareInterface::isRecordingRunning() {
    LOGI("isRecordingRunning: E");
    bool ret = false;
    if((mCameraState == CAMERA_STATE_RECORD) || (mCameraState == CAMERA_STATE_RECORD_START_CMD_SENT)) {
       return true;
    }
    LOGI("isRecordingRunning: X");
    return ret;
}

bool QCameraHardwareInterface::isSnapshotRunning() {
    LOGI("isSnapshotRunning: E");
    bool ret = false;
    if((mCameraState == CAMERA_STATE_SNAP_CMD_ACKED) || (mCameraState == CAMERA_STATE_SNAP_START_CMD_SENT)) {
        return true;
    }
    return ret;
    LOGI("isSnapshotRunning: X");
}

void QCameraHardwareInterface::debugShowPreviewFPS() const
{
    static int mFrameCount;
    static int mLastFrameCount = 0;
    static nsecs_t mLastFpsTime = 0;
    static float mFps = 0;
    mFrameCount++;
    nsecs_t now = systemTime();
    nsecs_t diff = now - mLastFpsTime;
    if (diff > ms2ns(250)) {
        mFps =  ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
        LOGI("Preview Frames Per Second: %.4f", mFps);
        mLastFpsTime = now;
        mLastFrameCount = mFrameCount;
    }
}

void QCameraHardwareInterface::processPreviewChannelEvent(mm_camera_ch_event_type_t channelEvent) {
    LOGI("processPreviewChannelEvent: E");
    Mutex::Autolock lock(mLock);
    switch(channelEvent) {
        case MM_CAMERA_CH_EVT_STREAMMING_ON:
            mCameraState = CAMERA_STATE_PREVIEW;
            break;
        case MM_CAMERA_CH_EVT_STREAMMING_OFF:
            mCameraState = CAMERA_STATE_READY;
            break;
        case MM_CAMERA_CH_EVT_DATA_DELIVERY_DONE:
            break;
        default:
            break;
    }
    LOGI("processPreviewChannelEvent: X");
    return;
}

void QCameraHardwareInterface::processRecordChannelEvent(mm_camera_ch_event_type_t channelEvent) {
    LOGI("processRecordChannelEvent: E");
    Mutex::Autolock lock(mLock);
    switch(channelEvent) {
        case MM_CAMERA_CH_EVT_STREAMMING_ON:
            mCameraState = CAMERA_STATE_RECORD;
            break;
        case MM_CAMERA_CH_EVT_STREAMMING_OFF:
            mCameraState = CAMERA_STATE_PREVIEW;
            break;
        case MM_CAMERA_CH_EVT_DATA_DELIVERY_DONE:
            break;
        default:
            break;
    }
    LOGI("processRecordChannelEvent: X");
    return;
}

void QCameraHardwareInterface::processSnapshotChannelEvent(mm_camera_ch_event_type_t channelEvent) {
    LOGI("processSnapshotChannelEvent: E");
    Mutex::Autolock lock(mLock);
    switch(channelEvent) {
        case MM_CAMERA_CH_EVT_STREAMMING_ON:
            mCameraState = CAMERA_STATE_SNAP_CMD_ACKED;
            break;
        case MM_CAMERA_CH_EVT_STREAMMING_OFF:
            mCameraState = CAMERA_STATE_READY;
            break;
        case MM_CAMERA_CH_EVT_DATA_DELIVERY_DONE:
            break;
        default:
            break;
    }
    LOGI("processSnapshotChannelEvent: X");
    return;
}

void QCameraHardwareInterface::processChannelEvent(mm_camera_ch_event_t *event)
{
    LOGI("processChannelEvent: E");
    switch(event->ch) {
        case MM_CAMERA_CH_PREVIEW:
            processPreviewChannelEvent(event->evt);
            break;
        case MM_CAMERA_CH_VIDEO:
            processRecordChannelEvent(event->evt);
            break;
        case MM_CAMERA_CH_SNAPSHOT:
            processSnapshotChannelEvent(event->evt);
            break;
        default:
            break;
    }
    LOGI("processChannelEvent: X");
    return;
}

void QCameraHardwareInterface::processCtrlEvent(mm_camera_ctrl_event_t *event)
{
    LOGI("processCtrlEvent: %d, E",event->evt);
    switch(event->evt)
    {
        case MM_CAMERA_CTRL_EVT_ZOOM_DONE:
            zoomEvent(&event->status);
            break;
        case MM_CAMERA_CTRL_EVT_AUTO_FOCUS_DONE:
            autoFocusEvent(&event->status);
            break;
        case MM_CAMERA_CTRL_EVT_PREP_SNAPSHOT:
            break;
        default:
            break;
    }
    LOGI("processCtrlEvent: X");
    return;
}
void  QCameraHardwareInterface::processStatsEvent(mm_camera_stats_event_t *event)
{
  return;
}

void  QCameraHardwareInterface::processEvent(mm_camera_event_t *event)
{
    LOGI("processEvent: E");
    switch(event->event_type)
    {
        case MM_CAMERA_EVT_TYPE_CH:
            processChannelEvent(&event->e.ch);
            break;
        case MM_CAMERA_EVT_TYPE_CTRL:
            processCtrlEvent(&event->e.ctrl);
            break;
        case MM_CAMERA_EVT_TYPE_STATS:
            processStatsEvent(&event->e.stats);
            break;
        default:
            break;
    }
    LOGI("processEvent: X");
    return;
}

bool QCameraHardwareInterface::preview_parm_config (cam_ctrl_dimension_t* dim,
                                   CameraParameters& parm)
{
    LOGI("preview_parm_config: E");
    bool matching = true;
    int display_width = 0;  /* width of display      */
    int display_height = 0; /* height of display */
    uint16_t video_width = 0;  /* width of the video  */
    uint16_t video_height = 0; /* height of the video */
    const char *str = parm.getPreviewFormat();

    /* First check if the preview resolution is the same, if not, change it*/
    parm.getPreviewSize(&display_width,  &display_height);
    if (display_width && display_height) {
        matching = (display_width == dim->display_width) &&
            (display_height == dim->display_height);

        if (!matching) {
            dim->display_width  = display_width;
            dim->display_height = display_height;
        }
    }
    else
        matching = false;

    cam_format_t value = getPreviewFormat();

    if(value != NOT_FOUND && value != dim->prev_format ) {
        //Setting to Parameter requested by the Upper layer
        dim->prev_format = value;
    }else{
        //Setting to default Format.
        dim->prev_format = CAMERA_YUV_420_NV21;
    }

    dim->enc_format = CAMERA_YUV_420_NV12;
    dim->orig_video_width = mDimension.orig_video_width;
    dim->orig_video_height = mDimension.orig_video_height;
    dim->video_width = mDimension.video_width;
    dim->video_height = mDimension.video_height;
    dim->video_chroma_width = mDimension.video_width;
    dim->video_chroma_height  = mDimension.video_height;

    LOGI("preview_parm_config: X");
    return matching;
}

status_t QCameraHardwareInterface::startPreview()
{
    LOGI("startPreview: E");
    Mutex::Autolock lock(mLock);
    status_t ret = NO_ERROR;

    cam_ctrl_dimension_t dim;
    bool initPreview = false;

    if (isPreviewRunning()){
        LOGE("%s:Preview already started  mCameraState = %d!", __func__, mCameraState);
        LOGE("%s: X", __func__);
        return NO_ERROR;
    }

    if (!mmCamera) {
        LOGE("%s: error - native camera is NULL!", __func__);
        LOGE("%s: X", __func__);
        return BAD_VALUE;
    }
    else{
      /* yyan : get existing preview information, by qury mm_camera*/
      memset(&dim, 0, sizeof(cam_ctrl_dimension_t));
      ret = mmCamera->cfg->get_parm(mmCamera, MM_CAMERA_PARM_DIMENSION,&dim);
    }

    if (MM_CAMERA_OK != ret) {
      LOGE("%s: error - can't get preview dimension!", __func__);
      LOGE("%s: X", __func__);
      return BAD_VALUE;
    }

    if (mPrevForPostviewBuf.frame[0].buffer != NULL) {
        mm_camera_do_munmap(mPrevForPostviewBuf.frame[0].fd,
                            (void *)mPrevForPostviewBuf.frame[0].buffer,
                            mPrevForPostviewBuf.frame_len);
        memset(&mPrevForPostviewBuf, 0, sizeof(mPrevForPostviewBuf));
    }


    /* config the parmeters and see if we need to re-init the stream*/
    initPreview = preview_parm_config (&dim, mParameters);
    ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_DIMENSION,&dim);
    if (MM_CAMERA_OK != ret) {
      LOGE("%s: error - can't config preview parms!", __func__);
      LOGE("%s: X", __func__);
      return BAD_VALUE;
    }

    /*yyan: if stream object exists but it needs to re-init, delete it now.
       otherwise just call mPreviewStream->start() later ,
      */
    if (mStreamDisplay && initPreview){
      LOGE("%s:Deleting old stream instance",__func__);
      QCameraStream_preview::deleteInstance (mStreamDisplay);
      mStreamDisplay = NULL;
    }

    /*yyan:, if stream object doesn't exists, create and init it now.
       and call mPreviewStream->start() later ,
      */
    if (!mStreamDisplay){
      mStreamDisplay = QCameraStream_preview::createInstance(mmCamera,
                                                      CAMERA_MODE_2D);
      LOGE("%s:Creating new stream instance",__func__);
    }

    if (!mStreamDisplay) {
      LOGE("%s: error - can't creat preview stream!", __func__);
      return BAD_VALUE;
    }

    mStreamDisplay->setHALCameraControl(this);

    /*yyan TODO : now init all the buffers and send to steam object*/

    ret = mStreamDisplay->init();
    if (MM_CAMERA_OK != ret){
      LOGE("%s: error - can't init preview stream!", __func__);
      LOGE("%s: X", __func__);
      /* yyan TODO: shall we delete it? */
      return BAD_VALUE;
    }

    ret = mStreamDisplay->start();
    /* yyan TODO: call QCameraStream_noneZSL::start() */
    if (MM_CAMERA_OK != ret){
      LOGE("%s: error - can't start nonZSL stream!", __func__);
      LOGE("%s: X", __func__);
      return BAD_VALUE;
    }
    if(MM_CAMERA_OK == ret)
        mCameraState = CAMERA_STATE_PREVIEW_START_CMD_SENT;
    else
        mCameraState = CAMERA_STATE_ERROR;

    LOGI("startPreview: X");
    return ret;
}

void QCameraHardwareInterface::stopPreview()
{
    LOGI("stopPreview: E");
    Mutex::Autolock lock(mLock);
    if (isPreviewRunning()) {
        stopPreviewInternal();
    } else {
        LOGE("%s: Preview already stopped",__func__);
    }
    LOGI("stopPreview: X");
}

void QCameraHardwareInterface::stopPreviewInternal()
{
    LOGI("stopPreviewInternal: E");
    status_t ret = NO_ERROR;

    if(!mStreamDisplay) {
        LOGE("mStreamDisplay is null");
        return;
    }

    /* yyan : call QCameraStream_noneZSL::stop() second*/
    mStreamDisplay->stop();

    /* yyan: call QCameraStream_noneZSL::relase() last*/
    mStreamDisplay->release();

    /* Delete mStreamDisplay instance*/
    QCameraStream_preview::deleteInstance (mStreamDisplay);
    mStreamDisplay = NULL;

    mCameraState = CAMERA_STATE_PREVIEW_STOP_CMD_SENT;
    LOGI("stopPreviewInternal: X");
}

bool QCameraHardwareInterface::previewEnabled()
{
    LOGI("previewEnabled: E");
    Mutex::Autolock lock(mLock);
    LOGV("%s: mCameraState = %d", __func__, mCameraState);
    LOGI("previewEnabled: X");

    //TBD - Need to check on target if this flag is enough
    return isPreviewRunning();
}

status_t QCameraHardwareInterface::startRecording()
{
    LOGI("startRecording: E");
    status_t ret = NO_ERROR;
    Mutex::Autolock lock(mLock);
    if (isRecordingRunning()) {
        LOGV("%s: X - record already running", __func__);
        return NO_ERROR;
    }

    /*if((mCameraState != CAMERA_STATE_PREVIEW) //TODO : Need to enable once evt cb working
            || (mCameraState != CAMERA_STATE_PREVIEW_START_CMD_SENT))
    {
        LOGE("%s:Preview is not Initialized. Cannot start recording",__func__);
        return NO_ERROR;
    }*/

    if (mStreamRecord){
        LOGE("%s:Deleting old Record stream instance",__func__);
        QCameraStream_record::deleteInstance (mStreamRecord);
        mStreamRecord = NULL;
    }

    /*
     * Creating Instance of record stream.
     */
    mStreamRecord = QCameraStream_record::createInstance(mmCamera,
            CAMERA_MODE_2D);

    if (!mStreamRecord) {
        LOGE("%s: error - can't creat record stream!", __func__);
        return BAD_VALUE;
    }

    /* Store HAL object in record stream Object */
    mStreamRecord->setHALCameraControl(this);

    /*
     * now init encode buffers and send to steam object
     */
    //ret =  mStreamRecord->initEncodeBuffers();
    if (NO_ERROR == ret) {
        /*
         * call Record init()
         * Buffer Allocation, Channel acquire, Prepare Buff
         */
        ret =  mStreamRecord->init();
        if (MM_CAMERA_OK != ret){
            LOGE("%s: error - can't init Record stream!", __func__);
            return BAD_VALUE;
        }
    }else {
        LOGE("%s: error - can't allocate Encode Buffers!", __func__);
    }

    /*
     * call Record start() :
     * Register Callback, action start
     */
    ret =  mStreamRecord->start();
    if (MM_CAMERA_OK != ret){
        LOGE("%s: error - can't start nonZSL stream!", __func__);
        return BAD_VALUE;
    }

    if(MM_CAMERA_OK == ret)
        mCameraState = CAMERA_STATE_RECORD_START_CMD_SENT;
    else
        mCameraState = CAMERA_STATE_ERROR;

    LOGI("startRecording: X");
    return ret;
}

void QCameraHardwareInterface::stopRecording()
{
    LOGI("stopRecording: E");
    Mutex::Autolock lock(mLock);
    if (isRecordingRunning()) {
        stopRecordingInternal();
    } else {
        LOGE("%s: Recording already stopped",__func__);
    }
    LOGI("stopRecording: X");

}
void QCameraHardwareInterface::stopRecordingInternal()
{
    LOGI("stopRecordingInternal: E");
    status_t ret = NO_ERROR;

    if(!mStreamRecord) {
        LOGE("mStreamRecord is null");
        return;
    }

    /*
     * call QCameraStream_record::stop()
     * Unregister Callback, action stop
     */
    mStreamRecord->stop();

    /*
     * call QCameraStream_record::release()
     * Buffer dellocation, Channel release, UnPrepare Buff
     */
    mStreamRecord->release();

    QCameraStream_record::deleteInstance (mStreamRecord);
    mStreamRecord = NULL;

  //mCameraState = CAMERA_STATE_RECORD_STOP_CMD_SENT;
  mCameraState = CAMERA_STATE_PREVIEW;  //TODO : Apurva : Hacked for 2nd time Recording

    LOGI("stopRecordingInternal: X");
    return;
}

bool QCameraHardwareInterface::recordingEnabled()
{
  Mutex::Autolock lock(mLock);
  LOGV("%s: E", __func__);
  return isRecordingRunning();
  LOGV("%s: X", __func__);
}

/**
* Release a record frame previously returned by CAMERA_MSG_VIDEO_FRAME.
*/

void QCameraHardwareInterface::releaseRecordingFrame(const sp<IMemory>& mem)
{
  LOGE("%s : BEGIN",__func__);
  if(mStreamRecord == NULL) {
    LOGE("Record stream Not Initialized");
    return;
  }
  mStreamRecord->releaseRecordingFrame(mem);
  LOGE("%s : END",__func__);
  return;
}

status_t QCameraHardwareInterface::autoFocusEvent(cam_ctrl_status_t *status)
{
    LOGE("autoFocusEvent: E");
    int ret = NO_ERROR;

    Mutex::Autolock lock(mLock);


/**************************************************************
  BEGIN MUTEX CODE
  *************************************************************/

    LOGE("%s:%d: Trying to acquire AF bit lock",__func__,__LINE__);
    mAutofocusLock.lock();
    LOGE("%s:%d: Acquired AF bit lock",__func__,__LINE__);

    if(mAutoFocusRunning==false) {
      LOGE("%s:AF not running, discarding stale event",__func__);
      mAutofocusLock.unlock();
      return ret;
    }

    mAutoFocusRunning = false;
    mAutofocusLock.unlock();

/**************************************************************
  END MUTEX CODE
  *************************************************************/


    if(status==NULL) {
      LOGE("%s:NULL ptr received for status",__func__);
      return BAD_VALUE;
    }





    /*(Do?) we need to make sure that the call back is the
      last possible step in the execution flow since the same
      context might be used if a fail triggers another round
      of AF then the mAutoFocusRunning flag and other state
      variables' validity will be under question*/

    if (mNotifyCb && ( mMsgEnabled & CAMERA_MSG_FOCUS)){
      LOGE("%s:Issuing callback to service",__func__);

      /* "Accepted" status is not appropriate it should be used for
        initial cmd, event reporting should only give use SUCCESS/FAIL
        */
      if(*status==CAM_CTRL_SUCCESS || *status==CAM_CTRL_ACCEPTED) {
        mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);
      }
      else if(*status==CAM_CTRL_FAILED){
        mNotifyCb(CAMERA_MSG_FOCUS, false, 0, mCallbackCookie);
      }
      else{
        LOGE("%s:Unknown AF status (%d) received",__func__,*status);
      }


    }/*(mNotifyCb && ( mMsgEnabled & CAMERA_MSG_FOCUS))*/
    else{
      LOGE("%s:Call back not enabled",__func__);
    }



    LOGE("autoFocusEvent: X");
    return ret;

}

status_t QCameraHardwareInterface::cancelPicture()
{
    LOGI("cancelPicture: E");
    status_t ret = MM_CAMERA_OK;
    Mutex::Autolock lock(mLock);

    ret = cancelPictureInternal();
    LOGI("cancelPicture: X");
    return ret;
}

status_t QCameraHardwareInterface::cancelPictureInternal()
{
    LOGI("cancelPictureInternal: E");
    status_t ret = MM_CAMERA_OK;
    if(mCameraState != CAMERA_STATE_READY) {
        if(mStreamSnap) {
            mStreamSnap->stop();
            mCameraState = CAMERA_STATE_SNAP_STOP_CMD_SENT;
        }
    } else {
        LOGE("%s: Cannot process cancel picture as snapshot is already done",__func__);
    }
    LOGI("cancelPictureInternal: X");
    return ret;
}

status_t  QCameraHardwareInterface::takePicture()
{
    LOGI("takePicture: E");
    status_t ret = MM_CAMERA_OK;
    Mutex::Autolock lock(mLock);

    /* TBD: disable this check till event is properly handled*/
/*    if (isSnapshotRunning()) {
        LOGV("%s: X - Snapshot already running", __func__);
        return NO_ERROR;
    }

    if((mCameraState != CAMERA_STATE_PREVIEW)
            || (mCameraState != CAMERA_STATE_PREVIEW_START_CMD_SENT))
    {
        LOGE("%s:Preview is not Initialized. Cannot take picture",__func__);
        return NO_ERROR;
    }
*/
    if (mStreamSnap){
        LOGE("%s:Deleting old Snapshot stream instance",__func__);
        QCameraStream_Snapshot::deleteInstance (mStreamSnap);
        mStreamSnap = NULL;
    }

    /*
     * Creating Instance of snapshot stream.
     */
    mStreamSnap = QCameraStream_Snapshot::createInstance(mmCamera,
                                                       myMode);

    if (!mStreamSnap) {
        LOGE("%s: error - can't creat snapshot stream!", __func__);
        return BAD_VALUE;
    }

    /* Store HAL object in snapshot stream Object */
    mStreamSnap->setHALCameraControl(this);

    /* Call prepareSnapshot before stopping preview */
    mStreamSnap->prepareHardware();

    /* There's an issue where we have a glimpse of corrupted data between
       a time we stop a preview and display the postview. It happens because
       when we call stopPreview we deallocate the preview buffers hence overlay
       displays garbage value till we enqueue postview buffer to be displayed.
       Hence for temporary fix, we'll do memcopy of the last frame displayed and
       queue it to overlay*/
    storePreviewFrameForPostview();

    /* stop preview */
    stopPreviewInternal();

    /* Call snapshot init*/
    ret =  mStreamSnap->init();
    if (MM_CAMERA_OK != ret){
        LOGE("%s: error - can't init Snapshot stream!", __func__);
        return BAD_VALUE;
    }

    /* call Snapshot start() :*/
    ret =  mStreamSnap->start();
    if (MM_CAMERA_OK != ret){
        LOGE("%s: error - can't start Snapshot stream!", __func__);
        return BAD_VALUE;
    }

    if(MM_CAMERA_OK == ret)
        mCameraState = CAMERA_STATE_SNAP_START_CMD_SENT;
    else
        mCameraState = CAMERA_STATE_ERROR;

    LOGI("takePicture: X");
    return ret;
}

void  QCameraHardwareInterface::encodeData()
{
    LOGI("encodeData: E");
    LOGI("encodeData: X");
}

status_t  QCameraHardwareInterface::takeLiveSnapshot()
{
    status_t ret = MM_CAMERA_OK;
    LOGI("takeLiveSnapshot: E");

    LOGI("takeLiveSnapshot: X");
    return ret;
}

status_t QCameraHardwareInterface::autoFocus()
{
    LOGI("autoFocus: E");
    status_t ret = NO_ERROR;
    Mutex::Autolock lock(mLock);
    LOGI("autoFocus: Got lock");
    bool status = true;
    isp3a_af_mode_t afMode = getAutoFocusMode(mParameters);

    if(mAutoFocusRunning==true){
      LOGE("%s:AF already running should not have got this call",__func__);
      return UNKNOWN_ERROR;
    }

    // Skip autofocus if focus mode is infinity.
    if (afMode == AF_MODE_MAX ) {
      LOGE("%s:Invalid AF mode (%d)",__func__,afMode);

      /*Issue a call back to service that AF failed
      or the upper layer app might hang waiting for a callback msg*/
      if (mNotifyCb && ( mMsgEnabled & CAMERA_MSG_FOCUS)){
          mNotifyCb(CAMERA_MSG_FOCUS, false, 0, mCallbackCookie);
      }
      return UNKNOWN_ERROR;
    }


    LOGI("%s:AF start (mode %d)",__func__,afMode);
    if(MM_CAMERA_OK!=mmCamera->ops->action(mmCamera,TRUE,MM_CAMERA_OPS_FOCUS,&afMode )) {
      LOGE("%s: AF command failed err:%d error %s",__func__, errno,strerror(errno));
      return UNKNOWN_ERROR;
    }

    mAutoFocusRunning = true;
    LOGI("autoFocus: X");
    return ret;
}

status_t QCameraHardwareInterface::cancelAutoFocus()
{
    LOGE("cancelAutoFocus: E");
    status_t ret = NO_ERROR;
    Mutex::Autolock lock(mLock);

/**************************************************************
  BEGIN MUTEX CODE
*************************************************************/

    mAutofocusLock.lock();
    if(mAutoFocusRunning) {

      mAutoFocusRunning = false;
      mAutofocusLock.unlock();

    }else/*(!mAutoFocusRunning)*/{

      mAutofocusLock.unlock();
      LOGE("%s:Af not running",__func__);
      return NO_ERROR;
    }
/**************************************************************
  END MUTEX CODE
*************************************************************/


    if(MM_CAMERA_OK!=mmCamera->ops->action(mmCamera,FALSE,MM_CAMERA_OPS_FOCUS,NULL )) {
      LOGE("%s: AF command failed err:%d error %s",__func__, errno,strerror(errno));
    }

    LOGE("cancelAutoFocus: X");
    return NO_ERROR;
}
/*==========================================================================
 * FUNCTION    - prepareSnapshotAndWait -
 *
 * DESCRIPTION:  invoke preparesnapshot and wait for it done
                 it can be called within takepicture, so no need
                 to grab mLock.
 *=========================================================================*/
void QCameraHardwareInterface::prepareSnapshotAndWait()
{
    LOGI("prepareSnapshotAndWait: E");
    int rc = 0;
    /*To Do: call mm camera preparesnapshot */
    if(!rc ) {
        mPreparingSnapshot = true;
        pthread_mutex_lock(&mAsyncCmdMutex);
        pthread_cond_wait(&mAsyncCmdWait, &mAsyncCmdMutex);
        pthread_mutex_unlock(&mAsyncCmdMutex);
        mPreparingSnapshot = false;
    }
    LOGI("prepareSnapshotAndWait: X");
}

/*==========================================================================
 * FUNCTION    - processprepareSnapshotEvent -
 *
 * DESCRIPTION:  Process the event of preparesnapshot done msg
                 unblock prepareSnapshotAndWait( )
 *=========================================================================*/
void QCameraHardwareInterface::processprepareSnapshotEvent(cam_ctrl_status_t *status)
{
    LOGI("processprepareSnapshotEvent: E");
    pthread_mutex_lock(&mAsyncCmdMutex);
    pthread_cond_signal(&mAsyncCmdWait);
    pthread_mutex_unlock(&mAsyncCmdMutex);
    LOGI("processprepareSnapshotEvent: X");
}


void QCameraHardwareInterface::zoomEvent(cam_ctrl_status_t *status)
{
    LOGE("zoomEvent: state:%d E",mCameraState);
    Mutex::Autolock lock(mLock);
    switch(mCameraState ) {
      case CAMERA_STATE_SNAP_CMD_ACKED:

        memset(&v4l2_crop,0,sizeof(v4l2_crop));
        v4l2_crop.ch_type=MM_CAMERA_CH_SNAPSHOT;
        LOGI("Fetching crop info");
        mmCamera->cfg->get_parm(mmCamera,MM_CAMERA_PARM_CROP,&v4l2_crop);
        LOGI("Back from get parm");
        LOGI("Crop info received for main: %d, %d, %d, %d ",v4l2_crop.snapshot.main_crop.left,v4l2_crop.snapshot.main_crop.top,v4l2_crop.snapshot.main_crop.width,v4l2_crop.snapshot.main_crop.height);
        LOGI("Crop info received for main: %d, %d, %d, %d ",v4l2_crop.snapshot.thumbnail_crop.left,v4l2_crop.snapshot.thumbnail_crop.top,v4l2_crop.snapshot.thumbnail_crop.width,v4l2_crop.snapshot.thumbnail_crop.height);
        if(mStreamSnap) {
          mStreamSnap->mCrop=v4l2_crop;
        }

        break;

        case CAMERA_STATE_PREVIEW:
        case CAMERA_STATE_RECORD_START_CMD_SENT:
        case CAMERA_STATE_RECORD:
        default:

        #if 0
            if(mSmoothZoomRunning) {
                if (mCurrentZoom == mTargetSmoothZoom) {
                    mSmoothZoomRunning = false;
                    mNotifyCb(CAMERA_MSG_ZOOM,
                            mCurrentZoom, 1, mCallbackCookie);
                } else {
                    mCurrentZoom += mSmoothZoomStep;
                    if ((mSmoothZoomStep < 0 && mCurrentZoom < mTargetSmoothZoom)||
                            (mSmoothZoomStep > 0 && mCurrentZoom > mTargetSmoothZoom )) {
                        mCurrentZoom = mTargetSmoothZoom;
                    }
                    mParameters.set("zoom", mCurrentZoom);
                    setZoom(mParameters);
                }
            } else
            #endif
            { /*regular zooming or smooth zoom stopped*/

                memset(&v4l2_crop,0,sizeof(v4l2_crop));
                v4l2_crop.ch_type=MM_CAMERA_CH_PREVIEW;
                LOGE("Fetching crop info");
                mmCamera->cfg->get_parm(mmCamera,MM_CAMERA_PARM_CROP,&v4l2_crop);
                LOGE("Back from get parm");
                LOGE("Crop info received: %d, %d, %d, %d ",v4l2_crop.crop.left,v4l2_crop.crop.top,v4l2_crop.crop.width,v4l2_crop.crop.height);
                mOverlayLock.lock();
                if(mOverlay != NULL){
                  LOGE("Setting crop");
                  if(v4l2_crop.crop.width!=0 && v4l2_crop.crop.height!=0) {
                    mOverlay->setCrop(v4l2_crop.crop.left, v4l2_crop.crop.top,
                          v4l2_crop.crop.width, v4l2_crop.crop.height);
                                    LOGE("Done setting crop");
                  }else{
                      LOGE("Resetting crop");
                      mOverlay->setCrop(0, 0,
                          previewWidth, previewHeight);
                  }
                }
                mOverlayLock.unlock();
                LOGI("Calling notify to service");
                LOGI("Currrent zoom :%d",mCurrentZoom);

            }
            break;

    }
    LOGI("zoomEvent: X");
}

/* This is temporary solution to hide the garbage screen seen during
   snapshot between the time preview stops and postview is queued to
   overlay for display. We won't have this problem after honeycomb.*/
status_t QCameraHardwareInterface::storePreviewFrameForPostview(void)
{
    int width = 0;  /* width of channel  */
    int height = 0; /* height of channel */
    uint32_t frame_len = 0; /* frame planner length */
    int buffer_num = 4; /* number of buffers for display */
    uint32_t y_off=0;
    uint32_t cbcr_off=0;
    status_t ret = NO_ERROR;
    struct msm_frame *preview_frame;
     unsigned long buffer_addr = 0;

    LOGI("%s: E", __func__);

    if (mStreamDisplay == NULL) {
        ret = FAILED_TRANSACTION;
        goto end;
    }
    memset( &mPrevForPostviewBuf, 0, sizeof(mm_cameara_stream_buf_t));

    /* get preview size */
    getPreviewSize(&width, &height);
    LOGE("%s: Preview Size: %dX%d", __func__, width, height);

    frame_len = mm_camera_get_msm_frame_len(getPreviewFormat(),
                                            myMode,
                                            width,
                                            height,
                                            OUTPUT_TYPE_P,
                                            &y_off,
                                            &cbcr_off);
    mPrevForPostviewBuf.frame_len = frame_len;

    LOGE("%s: Frame Length calculated: %d", __func__, frame_len);

    /* allocate the memory */
    buffer_addr =
        (unsigned long) mm_camera_do_mmap(frame_len,
                                          &(mPrevForPostviewBuf.frame[0].fd));

    if (!buffer_addr) {
        LOGE("%s: Error allocating memory to store Preview for Postview.",
             __func__);
        ret = NO_MEMORY;
        goto end;
    }

    mPrevForPostviewBuf.frame[0].buffer = buffer_addr;

    LOGE("%s: Get last queued preview frame", __func__);
    preview_frame = (struct msm_frame *)mStreamDisplay->getLastQueuedFrame();
    if (preview_frame == NULL) {
        LOGE("%s: Error retrieving preview frame.", __func__);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* Copy the frame */
    LOGE("%s: Copy the frame buffer. buffer: %x  preview_buffer: %x",
         __func__, (uint32_t)mPrevForPostviewBuf.frame[0].buffer,
         (uint32_t)preview_frame->buffer);

    memcpy((void *)mPrevForPostviewBuf.frame[0].buffer,
           (const void *)preview_frame->buffer,
           frame_len);

    /* Display the frame */
    LOGE("%s: Queue the buffer for display.", __func__);
    mOverlayLock.lock();
    if (mOverlay != NULL) {
        mOverlay->setFd(mPrevForPostviewBuf.frame[0].fd);
        mOverlay->queueBuffer((void *)0);
    }
    mOverlayLock.unlock();

end:
    LOGI("%s: X", __func__);
    return ret;
}

void QCameraHardwareInterface::dumpFrameToFile(struct msm_frame* newFrame,
  HAL_cam_dump_frm_type_t frm_type)
{
  int32_t enabled = 0;
  int frm_num;
  uint32_t  skip_mode;
  char value[PROPERTY_VALUE_MAX];
  char buf[128];
  property_get("persist.camera.dumpimg", value, "0");
  enabled = atoi(value);


  LOGV(" newFrame =%p, frm_type = %d", newFrame, frm_type);
  if(enabled & HAL_DUMP_FRM_MASK_ALL) {
    if((enabled & frm_type) && newFrame) {
      frm_num = ((enabled & 0xffff0000) >> 16);
      if(frm_num == 0) frm_num = 10; /*default 10 frames*/
      if(frm_num > 256) frm_num = 256; /*256 buffers cycle around*/
      skip_mode = ((enabled & 0x0000ff00) >> 8);
      if(skip_mode == 0) skip_mode = 1; /*no -skip */

      if( mDumpSkipCnt % skip_mode == 0) {
        if (mDumpFrmCnt >= 0 && mDumpFrmCnt <= frm_num) {
          int w, h;
          cam_ctrl_dimension_t dim = mDimension;
          status_t ret = mmCamera->cfg->get_parm(mmCamera,
            MM_CAMERA_PARM_DIMENSION, &dim);
          int file_fd;
          switch (frm_type) {
          case  HAL_DUMP_FRM_PREVIEW:
            w = dim.display_width;
            h = dim.display_height;
            sprintf(buf, "/data/%dp_%dx%d.yuv", mDumpFrmCnt, w, h);
            file_fd = open(buf, O_RDWR | O_CREAT, 0777);
            break;
          case HAL_DUMP_FRM_VIDEO:
            w = dim.video_width;
            h = dim.video_height;
            sprintf(buf, "/data/%dv_%dx%d.yuv", mDumpFrmCnt, w, h);
            file_fd = open(buf, O_RDWR | O_CREAT, 0777);
            break;
          case HAL_DUMP_FRM_MAIN:
            w = dim.picture_width;
            h = dim.picture_height;
            sprintf(buf, "/data/%dm_%dx%d.yuv", mDumpFrmCnt, w, h);
            file_fd = open(buf, O_RDWR | O_CREAT, 0777);
            break;
          case HAL_DUMP_FRM_THUMBNAIL:
            w = dim.ui_thumbnail_width;
            h = dim.ui_thumbnail_height;
            sprintf(buf, "/data/%dt_%dx%d.yuv", mDumpFrmCnt, w, h);
            file_fd = open(buf, O_RDWR | O_CREAT, 0777);
            break;
          default:
            w = h = 0;
            file_fd = -1;
            break;
          }

          if (file_fd < 0) {
            LOGE("%s: cannot open file:type=%d\n", __func__, frm_type);
          } else {
            write(file_fd, (const void *)newFrame->buffer, w * h);
            write(file_fd, (const void *)
              (newFrame->buffer + newFrame->cbcr_off), w * h / 2);
            close(file_fd);
            LOGE("dump %s", buf);
          }
        } else if(frm_num == 256){
          mDumpFrmCnt = 0;
        }
        mDumpFrmCnt++;
      }
      mDumpSkipCnt++;
    }
  }  else {
    mDumpFrmCnt = 0;
  }
}

}; // namespace android
