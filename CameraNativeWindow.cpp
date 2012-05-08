#include "CameraNativeWindow.h"
// #include "nsDebug.h"
#include <stdio.h>

// enable debug logging by setting to 1
#define CNW_DEBUG 0
#if CNW_DEBUG
#define CNW_LOGD(...) {(void)printf_stderr(__VA_ARGS__);}
#else
#define CNW_LOGD(...) ((void)0)
#endif

// #define CNW_LOGE(...) {(void)printf_stderr(__VA_ARGS__);}
#define CNW_LOGE( ... ) { (void)fprintf( stderr, __VA_ARGS__ ); }

using namespace android;


CameraNativeWindow::CameraNativeWindow()
{
    CameraNativeWindow::init();
}

CameraNativeWindow::~CameraNativeWindow()
{
    freeAllBuffersLocked();
}

void CameraNativeWindow::init()
{
    // Initialize the ANativeWindow function pointers.
    ANativeWindow::setSwapInterval  = hook_setSwapInterval;
    ANativeWindow::dequeueBuffer    = hook_dequeueBuffer;
    ANativeWindow::cancelBuffer     = hook_cancelBuffer;
    ANativeWindow::lockBuffer       = hook_lockBuffer;
    ANativeWindow::queueBuffer      = hook_queueBuffer;
    ANativeWindow::query            = hook_query;
    ANativeWindow::perform          = hook_perform;

    mDefaultWidth = 0;
    mDefaultHeight = 0;
    mPixelFormat = 0;
    mUsage = 0;
    mTimestamp = NATIVE_WINDOW_TIMESTAMP_AUTO;
    mBufferCount = MIN_BUFFER_SLOTS;
    mFrameCounter = 0;
}


int CameraNativeWindow::hook_setSwapInterval(ANativeWindow* window, int interval)
{
    CameraNativeWindow* c = getSelf(window);
    return c->setSwapInterval(interval);
}

int CameraNativeWindow::hook_dequeueBuffer(ANativeWindow* window,
        ANativeWindowBuffer** buffer)
{
    CameraNativeWindow* c = getSelf(window);
    return c->dequeueBuffer(buffer);
}

int CameraNativeWindow::hook_cancelBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer)
{
    CameraNativeWindow* c = getSelf(window);
    return c->cancelBuffer(buffer);
}

int CameraNativeWindow::hook_lockBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer)
{
    CameraNativeWindow* c = getSelf(window);
    return c->lockBuffer(buffer);
}

int CameraNativeWindow::hook_queueBuffer(ANativeWindow* window,
        ANativeWindowBuffer* buffer)
{
    CameraNativeWindow* c = getSelf(window);
    return c->queueBuffer(buffer);
}

int CameraNativeWindow::hook_query(const ANativeWindow* window,
                                int what, int* value)
{
    const CameraNativeWindow* c = getSelf(window);
    return c->query(what, value);
}

int CameraNativeWindow::hook_perform(ANativeWindow* window, int operation, ...)
{
    va_list args;
    va_start(args, operation);
    CameraNativeWindow* c = getSelf(window);
    return c->perform(operation, args);
}

void CameraNativeWindow::freeBufferLocked(int i)
{
    if (mSlots[i].mGraphicBuffer != NULL) {
        mSlots[i].mGraphicBuffer.clear();
        mSlots[i].mGraphicBuffer = NULL;
    }
    mSlots[i].mBufferState = BufferSlot::FREE;
}

void CameraNativeWindow::freeAllBuffersLocked()
{
    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        freeBufferLocked(i);
    }
}

int CameraNativeWindow::setBufferCount(int bufferCount) {
    CNW_LOGD("setBufferCount: count=%d", bufferCount);
    Mutex::Autolock lock(mMutex);

    if (bufferCount > NUM_BUFFER_SLOTS) {
        CNW_LOGE("setBufferCount: bufferCount larger than slots available");
        return BAD_VALUE;
    }

    // special-case, nothing to do
    if (bufferCount == mBufferCount) {
        return OK;
    }

    if (bufferCount < MIN_BUFFER_SLOTS) {
        CNW_LOGE("setBufferCount: requested buffer count (%d) is less than "
                "minimum (%d)", bufferCount, MIN_BUFFER_SLOTS);
        return BAD_VALUE;
    }

    // Error out if the user has dequeued buffers
    for (int i=0 ; i<mBufferCount ; i++) {
        if (mSlots[i].mBufferState == BufferSlot::DEQUEUED) {
            CNW_LOGE("setBufferCount: client owns some buffers");
            return -EINVAL;
        }
    }

    if (bufferCount > mBufferCount) {
        // easy, we just have more buffers
        mBufferCount = bufferCount;
        mDequeueCondition.signal();
        return OK;
    }

    // reducing the number of buffers
    // here we're guaranteed that the client doesn't have dequeued buffers
    // and will release all of its buffer references.
    freeAllBuffersLocked();
    mBufferCount = bufferCount;
    mDequeueCondition.signal();
    return OK;
}

int CameraNativeWindow::dequeueBuffer(android_native_buffer_t** buffer)
{
    Mutex::Autolock lock(mMutex);

    int found = -1;
    int dequeuedCount = 0;
    bool tryAgain = true;

    CNW_LOGD("dequeueBuffer: E");
    while (tryAgain) {

        // look for a free buffer to give to the client
        found = INVALID_BUFFER_SLOT;
        dequeuedCount = 0;
        for (int i = 0; i < mBufferCount; i++) {
            const int state = mSlots[i].mBufferState;
            if (state == BufferSlot::DEQUEUED) {
                dequeuedCount++;
            }
            else if (state == BufferSlot::FREE) {
                /* We return the oldest of the free buffers to avoid
                 * stalling the producer if possible.  This is because
                 * the consumer may still have pending reads of the
                 * buffers in flight.
                 */
                bool isOlder = mSlots[i].mFrameNumber < mSlots[found].mFrameNumber;
                if (found < 0 || isOlder) {
                    found = i;
                }
            }
        }

#if 0 //XXX: Not sure if we need to do this check

        // See whether a buffer has been queued since the last
        // setBufferCount so we know whether to perform the
        // MIN_UNDEQUEUED_BUFFERS check below.
        if (dequeuedCount>0) {
            // make sure the client is not trying to dequeue more buffers
            // than allowed.
            const int avail = mBufferCount - (dequeuedCount);
            if (avail < MIN_UNDEQUEUED_BUFFERS) {
                CNW_LOGE("dequeueBuffer: MIN_UNDEQUEUED_BUFFERS=%d exceeded "
                    "(dequeued=%d)",
                    MIN_UNDEQUEUED_BUFFERS,
                    dequeuedCount);
                return -EBUSY;
            }
        }
#endif

        // we're in synchronous mode and didn't find a buffer, we need to
        // wait for some buffers to be consumed
        tryAgain = (found == INVALID_BUFFER_SLOT);
        if (tryAgain) {
            mDequeueCondition.wait(mMutex);
        }
    }

    if (found == INVALID_BUFFER_SLOT) {
        // This should not happen.
        CNW_LOGE("dequeueBuffer: no available buffer slots");
        return -EBUSY;
    }

    const int buf = found;

    // buffer is now in DEQUEUED
    mSlots[buf].mBufferState = BufferSlot::DEQUEUED;

    const sp<GraphicBuffer>& gbuf(mSlots[buf].mGraphicBuffer);

    if (gbuf == NULL) {
        status_t error;
        sp<GraphicBuffer> graphicBuffer( new GraphicBuffer( mDefaultWidth, mDefaultHeight, mPixelFormat, mUsage));
        error = graphicBuffer->initCheck();
        if (error != NO_ERROR) {
            CNW_LOGE("dequeueBuffer: createGraphicBuffer failed with error %d",error);
            return error;
        }
        mSlots[buf].mGraphicBuffer = graphicBuffer;
    }
    *buffer = mSlots[buf].mGraphicBuffer.get();

    CNW_LOGD("dequeueBuffer: returning slot=%d buf=%p ", buf,
            mSlots[buf].mGraphicBuffer->handle );

    CNW_LOGD("dequeueBuffer: X");
    return NO_ERROR;
}

int CameraNativeWindow::getSlotFromBufferLocked(
        android_native_buffer_t* buffer) const
{
    if (buffer == NULL) {
        CNW_LOGE("getSlotFromBufferLocked: encountered NULL buffer");
        return BAD_VALUE;
    }

    for (int i = 0; i < NUM_BUFFER_SLOTS; i++) {
        if (mSlots[i].mGraphicBuffer != NULL && mSlots[i].mGraphicBuffer->handle == buffer->handle) {
            return i;
        }
    }
    CNW_LOGE("getSlotFromBufferLocked: unknown buffer: %p", buffer->handle);
    return BAD_VALUE;
}

int CameraNativeWindow::queueBuffer(ANativeWindowBuffer* buffer)
{
    Mutex::Autolock lock(mMutex);
    CNW_LOGD("queueBuffer: E");
    int buf = getSlotFromBufferLocked(buffer);

    if (buf < 0 || buf >= mBufferCount) {
        CNW_LOGE("queueBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, buf);
        return -EINVAL;
    } else if (mSlots[buf].mBufferState != BufferSlot::DEQUEUED) {
        CNW_LOGE("queueBuffer: slot %d is not owned by the client "
                "(state=%d)", buf, mSlots[buf].mBufferState);
        return -EINVAL;
    }

    int64_t timestamp;
    if (mTimestamp == NATIVE_WINDOW_TIMESTAMP_AUTO) {
        timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
    } else {
        timestamp = mTimestamp;
    }

    //XXX:
    //mSlots[buf].mBufferState = BufferSlot::QUEUED;
    //Set the state to FREE as there are no operations on the queued buffer
    //And, so that the buffer can be dequeued when needed.
    mSlots[buf].mBufferState = BufferSlot::FREE;
    mSlots[buf].mTimestamp = timestamp;
    mFrameCounter++;
    mSlots[buf].mFrameNumber = mFrameCounter;

    mDequeueCondition.signal();
    CNW_LOGD("queueBuffer: X");

    return OK;
}

int CameraNativeWindow::lockBuffer(ANativeWindowBuffer* buffer)
{
    CNW_LOGD("CameraNativeWindow::lockBuffer");
    Mutex::Autolock lock(mMutex);
    //TODO: Need to implement this.
    return OK;
}

int CameraNativeWindow::cancelBuffer(ANativeWindowBuffer* buffer)
{
    Mutex::Autolock lock(mMutex);
    int buf = getSlotFromBufferLocked(buffer);

    CNW_LOGD("cancelBuffer: slot=%d", buf);
    if (buf < 0 || buf >= mBufferCount) {
        CNW_LOGE("cancelBuffer: slot index out of range [0, %d]: %d",
                mBufferCount, buf);
        return -EINVAL;
    } else if (mSlots[buf].mBufferState != BufferSlot::DEQUEUED) {
        CNW_LOGE("cancelBuffer: slot %d is not owned by the client (state=%d)",
                buf, mSlots[buf].mBufferState);
        return -EINVAL;
    }
    mSlots[buf].mBufferState = BufferSlot::FREE;
    mSlots[buf].mFrameNumber = 0;
    mDequeueCondition.signal();
    return OK;
}

int CameraNativeWindow::perform(int operation, va_list args)
{
    int res = NO_ERROR;

    switch (operation) {
    case NATIVE_WINDOW_CONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_DISCONNECT:
        // deprecated. must return NO_ERROR.
        break;
    case NATIVE_WINDOW_SET_USAGE:
        res = dispatchSetUsage(args);
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        res = dispatchSetBufferCount(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        res = dispatchSetBuffersGeometry(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        res = dispatchSetBuffersTimestamp(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
        res = dispatchSetBuffersDimensions(args);
        break;
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        res = dispatchSetBuffersFormat(args);
        break;
    case NATIVE_WINDOW_SET_CROP:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_LOCK:
    case NATIVE_WINDOW_UNLOCK_AND_POST:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
    default:
        res = INVALID_OPERATION;
        break;
    }
    return res;
}

int CameraNativeWindow::query(int what, int* outValue) const
{
    Mutex::Autolock lock(mMutex);

    int value;
    switch (what) {
    case NATIVE_WINDOW_WIDTH:
        value = mDefaultWidth;
        break;
    case NATIVE_WINDOW_HEIGHT:
        value = mDefaultHeight;
        break;
    case NATIVE_WINDOW_FORMAT:
        value = mPixelFormat;
        break;
    case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
        value = MIN_UNDEQUEUED_BUFFERS;
        break;
    default:
        return BAD_VALUE;
    }
    outValue[0] = value;
    return NO_ERROR;
}

int CameraNativeWindow::setSwapInterval(int interval)
{
    return NO_ERROR;
}

//================================================
int CameraNativeWindow::dispatchSetUsage(va_list args)
{
    int usage = va_arg(args, int);
    return setUsage(usage);
}

int CameraNativeWindow::dispatchSetBufferCount(va_list args)
{
    size_t bufferCount = va_arg(args, size_t);
    return setBufferCount(bufferCount);
}

int CameraNativeWindow::dispatchSetBuffersGeometry(va_list args)
{
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    int f = va_arg(args, int);
    int err = setBuffersDimensions(w, h);
    if (err != 0) {
        return err;
    }
    return setBuffersFormat(f);
}

int CameraNativeWindow::dispatchSetBuffersDimensions(va_list args)
{
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    return setBuffersDimensions(w, h);
}

int CameraNativeWindow::dispatchSetBuffersFormat(va_list args)
{
    int f = va_arg(args, int);
    return setBuffersFormat(f);
}

int CameraNativeWindow::dispatchSetBuffersTimestamp(va_list args)
{
    int64_t timestamp = va_arg(args, int64_t);
    return setBuffersTimestamp(timestamp);
}

//================================================
int CameraNativeWindow::setUsage(uint32_t reqUsage)
{
    CNW_LOGD("CameraNativeWindow::setUsage");
    Mutex::Autolock lock(mMutex);
    mUsage = reqUsage;
    return OK;
}

int CameraNativeWindow::setBuffersDimensions(int w, int h)
{
    CNW_LOGD("CameraNativeWindow::setBuffersDimensions");
    Mutex::Autolock lock(mMutex);

    if (w<0 || h<0)
        return BAD_VALUE;

    if ((w && !h) || (!w && h))
        return BAD_VALUE;

    mDefaultWidth = w;
    mDefaultHeight = h;

    return OK;
}

int CameraNativeWindow::setBuffersFormat(int format)
{
    CNW_LOGD("CameraNativeWindow::setBuffersFormat");
    Mutex::Autolock lock(mMutex);

    if (format<0)
        return BAD_VALUE;

    mPixelFormat = format;

    return NO_ERROR;
}

int CameraNativeWindow::setBuffersTimestamp(int64_t timestamp)
{
    CNW_LOGD("CameraNativeWindow::setBuffersTimestamp");
    Mutex::Autolock lock(mMutex);
    mTimestamp = timestamp;
    return OK;
}
