/*
 * mikeh@mozilla.com
 * Let's exercise the libcamera API on ICS!
 */

#define LOG_TAG "snapshot/main.cpp"

#include <math.h>
#include <stdio.h>
#include <signal.h>
#include <libgen.h>     // for basename()
#include <unistd.h>
#include <pthread.h>
#include <utils/Log.h>
#include <hardware/camera.h>
#include <camera/CameraParameters.h>

#define USE_GS2_LIBCAMERA
#define CameraHardwareInterface CameraHardwareInterface_SGS2
#define HAL_openCameraHardware HAL_openCameraHardware_SGS2
#include "CameraHardwareInterface.h"
#undef CameraHardwareInterface
#undef USE_GS2_LIBCAMERA
#undef HAL_openCameraHardware
#undef ANDROID_HARDWARE_CAMERA_HARDWARE_INTERFACE_H

#define printf_stderr( ... ) fprintf( stderr, __VA_ARGS__ )
#define CameraHardwareInterface CameraHardwareInterface_ICS
#include "CameraHardwareInterfaceICS.h"
#undef CameraHardwareInterface

#include "CameraNativeWindow.h"

using namespace android;

/*
    Events framework
*/
typedef enum {
    NO_EVENT,
    PREVIEW_STARTED,
    AUTO_FOCUSED,
    IMAGE_CAPTURED,
    ABORT,
    ERROR
} CAM_EVENT;

static volatile CAM_EVENT   event   = NO_EVENT;
static pthread_mutex_t      lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t       cond    = PTHREAD_COND_INITIALIZER;

static void fireEvent( CAM_EVENT e )
{
    LOGD( "fireEvent %d", e );
    
    pthread_mutex_lock( &lock );
    event = e;
    pthread_cond_signal( &cond );
    pthread_mutex_unlock( &lock );
}

static void handleSigInt( int sig )
{
    switch( sig ) {
        case SIGINT:
            printf( "\nGot SIGINT, exiting...\n" );
            fireEvent( ABORT );
            break;
        
        default:
            printf( "\nGot signal %d\n", sig );
            break;
    }
}

typedef void (*notify_callback_t)( int32_t msgType, int32_t ext1, int32_t ext2, void* user );
#define NO_METADATA 1
#if NO_METADATA
typedef void (*data_callback_t)( int32_t msgType, const sp<IMemory> &dataPtr, void* user );
#else
typedef void (*data_callback_t)( int32_t msgType, const sp<IMemory> &dataPtr, camera_frame_metadata_t* metadata, void* user );
#endif
typedef void (*data_callback_timestamp_t)( nsecs_t timestamp, int32_t msgType, const sp<IMemory> &dataPtr, void* user );

static void snapshot_notify_callback( int32_t msgType, int32_t ext1, int32_t ext2, void* user )
{
    switch( msgType ) {
        case android::CAMERA_MSG_FOCUS:
            if( ext1 ) {
                LOGD( "Autofocus complete" );
                fireEvent( AUTO_FOCUSED );
            } else {
                LOGD( "Autofocus failed" );
                fireEvent( ERROR );
            }
            break;

        default:
            LOGD( "Unhandled notify_callback msgType: %d", msgType );
            break;
    }
}

#if NO_METADATA
static void snapshot_data_callback( int32_t msgType, const sp<IMemory> &dataPtr, void* user )
#else
static void snapshot_data_callback( int32_t msgType, const sp<IMemory> &dataPtr, camera_frame_metadata_t* metadata, void* user )
#endif
{
    static int previewFrames = 0;
    static bool previewStarted = false;

    switch( msgType ) {
        case android::CAMERA_MSG_PREVIEW_FRAME:
            previewFrames += 1;
            if( previewFrames == 30 ) {
                LOGD( "Got 30 preview frames" );
                previewFrames = 0;
            }
            if( !previewStarted ) {
                fireEvent( PREVIEW_STARTED );
                previewStarted = true;
            }
            break;
            
        case android::CAMERA_MSG_COMPRESSED_IMAGE:
            LOGD( "Got compressed image: data=%p, length=%d", dataPtr->pointer(), dataPtr->size() );
            fireEvent( IMAGE_CAPTURED );
            break;

        default:
            LOGD( "Unhandled data_callback msgType: %d", msgType );
            break;
    }
}

static void snapshot_data_callback_timestamp( nsecs_t timestamp, int32_t msgType, const sp<IMemory> &dataPtr, void* user )
{
    switch( msgType ) {
        default:
            LOGD( "Unhandled data_callback_timestamp msgType: %d", msgType );
            break;
    }
}

static CameraHardwareInterface_ICS* getCamera( camera_module_t* module, uint32_t whichOne )
{
    CameraHardwareInterface_ICS* camera;
    char camName[ 4 ];
    status_t s;

    snprintf( camName, sizeof( camName ), "%d", whichOne );
    camera = new CameraHardwareInterface_ICS( camName );
    if( ( s = camera->initialize( &module->common ) ) != OK ) {
        fprintf( stderr, "Unable to initialize camera: %d\n", s );
        LOGE( "Unable to initialize camera: %d", s );
        return NULL;
    }
    fprintf( stderr, "Camera initialized\n" );
    LOGD( "Camera initialized" );
    
    camera->setCallbacks( snapshot_notify_callback, snapshot_data_callback, snapshot_data_callback_timestamp, NULL );
    return camera;
}

static void dumpSupportedParameters( sp<CameraHardwareInterface_ICS> camera, uint32_t whichOne )
{
    CameraParameters p = camera->getParameters();
    
    fprintf( stderr, "Supported camera properties (camera %d):\n", whichOne );
    fprintf( stderr, "\tPreview sizes:                 %s\n", p.get( p.KEY_SUPPORTED_PREVIEW_SIZES ) );
    fprintf( stderr, "\tPreview FPS ranges:            %s\n", p.get( p.KEY_SUPPORTED_PREVIEW_FPS_RANGE ) );
    fprintf( stderr, "\tPreview formats:               %s\n", p.get( p.KEY_SUPPORTED_PREVIEW_FORMATS ) );
    fprintf( stderr, "\tPreview frame rates:           %s\n", p.get( p.KEY_SUPPORTED_PREVIEW_FRAME_RATES ) );
    fprintf( stderr, "\tPicture sizes:                 %s\n", p.get( p.KEY_SUPPORTED_PICTURE_SIZES ) );
    fprintf( stderr, "\tPicture formats:               %s\n", p.get( p.KEY_SUPPORTED_PICTURE_FORMATS ) );
    fprintf( stderr, "\tJPEG thumbnail sizes:          %s\n", p.get( p.KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES ) );
    fprintf( stderr, "\tWhite balances:                %s\n", p.get( p.KEY_SUPPORTED_WHITE_BALANCE ) );
    fprintf( stderr, "\tEffects:                       %s\n", p.get( p.KEY_SUPPORTED_EFFECTS ) );
    fprintf( stderr, "\tAnti-banding:                  %s\n", p.get( p.KEY_SUPPORTED_ANTIBANDING ) );
    fprintf( stderr, "\tScene modes:                   %s\n", p.get( p.KEY_SUPPORTED_SCENE_MODES ) );
    fprintf( stderr, "\tFlash modes:                   %s\n", p.get( p.KEY_SUPPORTED_FLASH_MODES ) );
    fprintf( stderr, "\tFocus modes:                   %s\n", p.get( p.KEY_SUPPORTED_FOCUS_MODES ) );
    fprintf( stderr, "\tFocal length:                  %s\n", p.get( p.KEY_FOCAL_LENGTH ) );
    fprintf( stderr, "\tHorizontal view angle:         %s\n", p.get( p.KEY_HORIZONTAL_VIEW_ANGLE ) );
    fprintf( stderr, "\tVertical view angle:           %s\n", p.get( p.KEY_VERTICAL_VIEW_ANGLE ) );
    fprintf( stderr, "\tMaximum exposure compensation: %s\n", p.get( p.KEY_MAX_EXPOSURE_COMPENSATION ) );
    fprintf( stderr, "\tMinimum exposure compensation: %s\n", p.get( p.KEY_MIN_EXPOSURE_COMPENSATION ) );
    fprintf( stderr, "\tExposure compensation step:    %s\n", p.get( p.KEY_EXPOSURE_COMPENSATION_STEP ) );
    fprintf( stderr, "\tMaximum zoom:                  %s\n", p.get( p.KEY_MAX_ZOOM ) );
    fprintf( stderr, "\tZoom ratios:                   %s\n", p.get( p.KEY_ZOOM_RATIOS ) );
    fprintf( stderr, "\tZoom supported:                %s\n", p.get( p.KEY_ZOOM_SUPPORTED ) );
    fprintf( stderr, "\tSmooth zoom supported:         %s\n", p.get( p.KEY_SMOOTH_ZOOM_SUPPORTED ) );
}

static void dumpCurrentParameters( sp<CameraHardwareInterface_ICS> camera, uint32_t whichOne )
{
    CameraParameters p = camera->getParameters();
    
    fprintf( stderr, "Current camera properties (camera %d):\n", whichOne );
    fprintf( stderr, "\tPreview size:                  %s\n", p.get( p.KEY_PREVIEW_SIZE ) );
    fprintf( stderr, "\tPreview FPS range:             %s\n", p.get( p.KEY_PREVIEW_FPS_RANGE ) );
    fprintf( stderr, "\tPreview format:                %s\n", p.get( p.KEY_PREVIEW_FORMAT ) );
    fprintf( stderr, "\tPreview frame rate:            %s\n", p.get( p.KEY_PREVIEW_FRAME_RATE ) );
    fprintf( stderr, "\tPicture size:                  %s\n", p.get( p.KEY_PICTURE_SIZE ) );
    fprintf( stderr, "\tPicture format:                %s\n", p.get( p.KEY_PICTURE_FORMAT ) );
    fprintf( stderr, "\tJPEG thumbnail width:          %s\n", p.get( p.KEY_JPEG_THUMBNAIL_WIDTH ) );
    fprintf( stderr, "\tJPEG thumbnail height:         %s\n", p.get( p.KEY_JPEG_THUMBNAIL_HEIGHT ) );
    fprintf( stderr, "\tWhite balance:                 %s\n", p.get( p.KEY_WHITE_BALANCE ) );
    fprintf( stderr, "\tEffect:                        %s\n", p.get( p.KEY_EFFECT ) );
    fprintf( stderr, "\tAnti-banding:                  %s\n", p.get( p.KEY_ANTIBANDING ) );
    fprintf( stderr, "\tScene mode:                    %s\n", p.get( p.KEY_SCENE_MODE ) );
    fprintf( stderr, "\tFlash mode:                    %s\n", p.get( p.KEY_FLASH_MODE ) );
    fprintf( stderr, "\tFocus mode:                    %s\n", p.get( p.KEY_FOCUS_MODE ) );
    fprintf( stderr, "\tFocal length:                  %s\n", p.get( p.KEY_FOCAL_LENGTH ) );
    fprintf( stderr, "\tHorizontal view angle:         %s\n", p.get( p.KEY_HORIZONTAL_VIEW_ANGLE ) );
    fprintf( stderr, "\tVertical view angle:           %s\n", p.get( p.KEY_VERTICAL_VIEW_ANGLE ) );
    fprintf( stderr, "\tMaximum exposure compensation: %s\n", p.get( p.KEY_MAX_EXPOSURE_COMPENSATION ) );
    fprintf( stderr, "\tMinimum exposure compensation: %s\n", p.get( p.KEY_MIN_EXPOSURE_COMPENSATION ) );
    fprintf( stderr, "\tExposure compensation step:    %s\n", p.get( p.KEY_EXPOSURE_COMPENSATION_STEP ) );
    fprintf( stderr, "\tMaximum zoom:                  %s\n", p.get( p.KEY_MAX_ZOOM ) );
    fprintf( stderr, "\tZoom:                          %s\n", p.get( p.KEY_ZOOM_SUPPORTED ) );
    fprintf( stderr, "\tSmooth zoom:                   %s\n", p.get( p.KEY_SMOOTH_ZOOM_SUPPORTED ) );
}

int main( int argc, char* argv[] )
{
    const char*                     program     = basename( argv[0] );
    camera_module_t*                module;
    sp<CameraHardwareInterface_ICS> camera;
    sp<ANativeWindow>               window      = new android::CameraNativeWindow();
    uint32_t                        whichOne    = android::CAMERA_FACING_BACK;
    status_t                        s;
    uint32_t                        count;
    const char*                     ofile       = "/system/data/snapshot.jpg";
    const char*                     effect      = CameraParameters::EFFECT_NONE;
    const char*                     flash       = CameraParameters::FLASH_MODE_AUTO;
    extern char*                    optarg;
    extern int                      optopt;
    extern int                      optind;
    int                             c;
    bool                            autoFocus   = true;
    
    signal( SIGINT, handleSigInt );
    
    fprintf( stderr, "--- %s [%s %s %s] ---\n", program, __FILE__, __DATE__, __TIME__ );
    LOGD( "---------- %s [%s %s %s] ----------\n", program, __FILE__, __DATE__, __TIME__ );
    
    while( ( c = getopt( argc, argv, ":e:f:no:" ) ) != -1 ) {
        switch( c ) {
            case 'e':
                effect = optarg;
                break;
            
            case 'f':
                flash = optarg;
                break;
            
            case 'o':
                ofile = optarg;
                break;
            
            case 'n':
                autoFocus = false;
                break;
            
            case '?':
                fprintf( stderr, "Unrecognized option: -%c\n", optopt );
                return 1;
        }
    }

    if( ( s = hw_get_module( CAMERA_HARDWARE_MODULE_ID, (const hw_module_t**)&module ) ) < 0 ) {
        fprintf( stderr, "Unable to get camera module: %d\n", s );
        LOGE( "Unable to get camera module: %d", s );
        return 1;
    }
    LOGD( "Got module: %p", module );
    
    count = module->get_number_of_cameras();
    if( count == 0 ) {
        LOGE( "No cameras found!" );
        fprintf( stderr, "No cameras found!" );
        return 1;
    }
    fprintf( stderr, "Number of cameras: %d\n", count );
    LOGD( "Number of cameras: %d", count );
    
    if( ( camera = getCamera( module, whichOne ) ) == NULL ) {
        fprintf( stderr, "Failed to get camera\n" );
        LOGE( "Failed to get camera" );
        return 1;
    }
    
    dumpSupportedParameters( camera, whichOne );
    
    /* Set parameters from command-line options */
    
    
    dumpCurrentParameters( camera, whichOne );

    /*
        Believe it or not, you MUST set a preview window in order
        to call startPreview(), even if you have no intention of
        using the returned data--or even if the camera doesn't write
        directly to the native window!
    */
    camera->setPreviewWindow( window );
    
    /*
        Believe it or not, you MUST call startPreview() before
        calling autoFocus(), or things just don't work.
        Nice going there, Android.
    */
    fprintf( stderr, "Starting preview..." );
    LOGD( "Starting preview..." );
    fflush( stderr );
    camera->enableMsgType( android::CAMERA_MSG_PREVIEW_FRAME );
    if( ( s = camera->startPreview() ) != OK ) {
        fprintf( stderr, "Unable to start preview: %d\n", s );
        LOGE( "Unable to start preview: %d", s );
        return 1;
    }
   
    LOGD( "----- Entering event loop -----" );
    bool exit = false;
    while( !exit ) {
        switch( event ) {
            case NO_EVENT:
                pthread_mutex_lock( &lock );
                pthread_cond_wait( &cond, &lock );
                LOGD( "Got event %d", event );
                continue;
            
            case ABORT:
                exit = true;
                break;
            
            case PREVIEW_STARTED:
                if( autoFocus ) {
                    camera->enableMsgType( android::CAMERA_MSG_FOCUS );
                    fprintf( stderr, "Starting autofocus..." );
                    LOGD( "Starting autofocus..." );
                    fflush( stderr );
                    if( ( s = camera->autoFocus() ) != OK ) {
                        fprintf( stderr, "failure (%d)\n", s );
                        LOGE( "Autofocus failed: %d", s );
                        return 1;
                    }
                    break;
                }
                // fallthrough if autoFocus is not set
            
            case AUTO_FOCUSED:
                camera->enableMsgType( android::CAMERA_MSG_COMPRESSED_IMAGE );
                fprintf( stderr, "OK\nTaking picture..." );
                LOGD( "Taking picture..." );
                fflush( stderr );
                if( ( s = camera->takePicture() ) != OK ) {
                    fprintf( stderr, "failure (%d)\n", s );
                    LOGE( "Take picture failed: %d", s );
                    return 1;
                }
                break;
            
            case IMAGE_CAPTURED:
                // save picture and exit
                fprintf( stderr, "OK\n" );
                LOGD( "Image captured" );
                
                exit = true;
                break;
            
            case ERROR:
                fprintf( stderr, "An error occured--check logcat\n" );
                return 1;
            
            default:
                fprintf( stderr, "Weird, unhandled event %d\n", event );
                break;
        }
        
        event = NO_EVENT;
        pthread_mutex_unlock( &lock );
    }
    LOGD( "----- Leaving event loop -----" );

    camera->stopPreview();
    camera->release();
    
    LOGD( "Done." );
    return 0;
}
