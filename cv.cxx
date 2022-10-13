// Create Video.
// Turns a set of images into an MP4 video.
// Lots of code from a Windows SDK sample were used.

#define UNICODE
#define USE_WIC_FOR_OPEN   // for open and resize, which is the big performance win

#include <Windows.h>
#include <winnt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <Mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <psapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <stdio.h>
#include <conio.h>
#include <process.h>
#include <math.h>
#include <ppl.h>

#include <chrono>
#include <memory>
#include <exception>

using namespace std;
using namespace std::chrono;
using namespace concurrency;
using namespace Gdiplus;

#include <djlenum.hxx>
#include <djltrace.hxx>

#ifdef USE_WIC_FOR_OPEN
    #include <djl_wic2gdi.hxx>
    #pragma comment( lib, "windowscodecs.lib" )
#endif

#pragma comment( lib, "mfreadwrite" )
#pragma comment( lib, "mfplat" )
#pragma comment( lib, "mfuuid" )
#pragma comment( lib, "ole32.lib" )
#pragma comment( lib, "Gdiplus.lib" )

template <class T> void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
} //SafeRelease

class CPerfTime
{
    private:
        LARGE_INTEGER liLastCall;
        LARGE_INTEGER liFrequency;
        NUMBERFMT NumberFormat;
        WCHAR awcRender[ 100 ];

    public:
        CPerfTime()
        {
            ZeroMemory( &NumberFormat, sizeof NumberFormat );
            NumberFormat.NumDigits = 0;
            NumberFormat.Grouping = 3;
            NumberFormat.lpDecimalSep = L".";
            NumberFormat.lpThousandSep = L",";

            Baseline();
            QueryPerformanceFrequency( &liFrequency );
        }

        void Baseline()
        {
            QueryPerformanceCounter( &liLastCall );
        }
    
        int RenderLL( LONGLONG ll, WCHAR * pwcBuf, ULONG cwcBuf )
        {
            WCHAR awc[100];
            swprintf( awc, L"%I64u", ll );

            if ( 0 != cwcBuf )
                *pwcBuf = 0;

            return GetNumberFormat( LOCALE_USER_DEFAULT, 0, awc, &NumberFormat, pwcBuf, cwcBuf );
        } //RenderLL

        WCHAR * RenderLL( LONGLONG ll )
        {
            WCHAR awc[100];
            swprintf( awc, L"%I64u", ll );

            awcRender[0] = 0;
            GetNumberFormat( LOCALE_USER_DEFAULT, 0, awc, &NumberFormat, awcRender, sizeof awcRender / sizeof WCHAR );
            return awcRender;
        } //RenderLL

        LONGLONG TimeSince()
        {
            LARGE_INTEGER liNow;
            QueryPerformanceCounter( &liNow );
            LONGLONG since = liNow.QuadPart - liLastCall.QuadPart;
            liLastCall = liNow;
            return since;
        }
    
        void CumulateSince( LONGLONG & running )
        {
            LARGE_INTEGER liNow;
            QueryPerformanceCounter( &liNow );
            LONGLONG since = liNow.QuadPart - liLastCall.QuadPart;
            liLastCall = liNow;

            InterlockedExchangeAdd64( &running, since );
        }
    
        LONGLONG TimeNow()
        {
            LARGE_INTEGER liNow;
            QueryPerformanceCounter( &liNow );
            return liNow.QuadPart;
        }

        LONGLONG Since( LONGLONG before )
        {
            LARGE_INTEGER liNow;
            QueryPerformanceCounter( &liNow );

            return liNow.QuadPart - before;
        }

        LONGLONG DurationToMS( LONGLONG duration )
        {
            duration *= 1000000;
            return ( duration / liFrequency.QuadPart ) / 1000;
        }

        LONGLONG NowToMS( LONGLONG startTime )
        {
            LONGLONG duration = TimeNow() - startTime;
            duration *= 1000000;
            return ( duration / liFrequency.QuadPart ) / 1000;
        }

        WCHAR * RenderDurationInMS( LONGLONG duration )
        {
            LONGLONG x = DurationToMS( duration );

            RenderLL( x, awcRender, sizeof awcRender / sizeof WCHAR );

            return awcRender;
        }
}; //CPerfTime

UINT32 g_video_bit_rate = 4000000;
byte g_fill_red = 0;
byte g_fill_green = 0;
byte g_fill_blue = 0;
UINT32 g_width = 1920;
UINT32 g_height = 1080;
WCHAR g_output_file[ MAX_PATH + 1 ] = {0};
WCHAR g_input_spec[ MAX_PATH + 1 ] = {0};
WCHAR g_input_text_file[ MAX_PATH + 1 ] = {0};
int g_parallelism = 4;
int g_transition = 0;
bool g_recurse = false;
bool g_stats = false;
bool g_usegpu = true;
bool g_captions = false;
UINT32 g_ms_delay = 1000;
UINT32 g_ms_transition_effect = 200;  // this is per entrance/exit. So a frame could have 2x total transition time.
std::mutex g_mtx;
CDJLTrace tracer;

// Format constants

// "all" here refers to stills loaded and fed to the video stream. 24 instead of 32 for size over speed in processing, but
// it's actually faster, since 24 is what's natively loaded by the JPG engine via GDI+ and what the MP4 codec expects.

const UINT32 ALL_BPP = 24;
const UINT32 ALL_BYTESPP = ALL_BPP / 8;

const UINT32 VIDEO_FPS = 24;
const GUID   VIDEO_ENCODING_FORMAT = MFVideoFormat_H264; // MFVideoFormat_HEVC works, but the results are almost identical
const GUID   VIDEO_INPUT_FORMAT = MFVideoFormat_RGB24;
const int    VIDEO_UNITS_PER_MS = 10000;


static void Usage()
{
    printf( "Usage: cv [input] /o:[outputname] /b:[Bitrate] /d:[Delay] /e:[EffectMS] /f:[0xBBGGRR] /w:[Width] /h:[Height] /i:[textfile] /p:[threads] /t:[1-5]\n" );
    printf( "  Create Video from a set of image files\n" );
    printf( "  arguments: [input]  Path with wildcard for input files. e.g. c:\\pics\\*.jpg\n" );
    printf( "             -b       Bitrate suggestion. Default is 4,000,000 bps\n" );
    printf( "             -d       Delay between each image in milliseconds. Default is 1000\n" );
    printf( "             -e       Milliseconds of transition Effect on enter/exit of a frame. Must be < 0.5 of /d. Default is 200\n" );
    printf( "             -f       Fill color RGB for portions of video a photo doesn't cover. Default is black 0x000000\n" );
    printf( "             -g       Disable use of GPU for rendering. By default, GPU will be used if available\n" );
    printf( "             -h       Height of the video (images are scaled then center-cropped to fit). Default is 1080\n" );
    printf( "             -i       Input text file with paths on each line. Alternative to using [input]\n" );
    printf( "             -o       Specifies the output file name. Overwrites existing file.\n" );
    printf( "             -p       Parallelism 1-16. If your images are small, try more. If out of RAM, try less. Default is 4\n" );
    printf( "             -r       Recurse into subdirectories looking for more images. Default is false\n" );
    printf( "             -s:X     Sort order of input images. Lowercase/Uppercase inverts order. WCUPR (write, create, capture, path, random)\n" );
    printf( "                      Default is random\n" );
    printf( "             -t       Add transitions between frames. Transitions types 1-2. Default none.\n" );
    printf( "             -w       Width of the video (images are scaled then center-cropped to fit). Default is 1920\n" );
    printf( "             -z       Stats: show detailed performance information\n" );
    printf( "  examples:  cv *.jpg /s:p /o:video.mp4 /d:500 /h:1920 /w:1080\n" );
    printf( "             cv *.jpg /s:C /o:video.mp4 /b:5000000 /h:512 /w:512\n" );
    printf( "             cv *.jpg /s:u /o:video.mp4 /b:5000000 /h:512 /w:512 /f:0x1300ac\n" );
    printf( "             cv *.jpg /o:video.mp4 /b:2500000 /h:512 /w:512 /p:1\n" );
    printf( "             cv *.jpg /o:video.mp4 /b:4000000 /h:2160 /w:3840 /p:16\n" );
    printf( "             cv d:\\pictures\\slothrust\\*.jpg /o:slothrust.mp4 /d:200\n" );
    printf( "             cv /t:1 d:\\pictures\\slothrust\\*.jpg /o:slothrust.mp4 /d:200\n" );
    printf( "             cv /f:0x33aa44 /h:1500 /w:1000 /o:y:\\shirt.mp4 d:\\shirt\\*.jpg /d:490 /p:6 -z -g\n" );
    printf( "             cv /f:0x33aa44 /h:1500 /w:1000 /o:y:\\shirt.mp4 d:\\shirt\\*.jpg /d:490 /p:16 -z\n" );
    printf( "             cv /f:0x000000 /h:1080 /w:1920 /o:y:\\2020.mp4 d:\\zdrive\\pics\\2020_wow\\*.jpg /d:4000 /t:1 /e:300 /p:8 -z\n" );
    printf( "  transitions:   1    Fade from/to black\n" );
    printf( "                 2    Fade from/to white\n" );

    exit( 1 );
} //Usage

static int StrideInBytes(int width, int bitsPerPixel)
{
    // Not sure if it's documented anywhere, but Windows seems to use 4-byte-aligned strides.
    // Stride is the number of bytes per row of an image. The last bytes to get to 4-byte alignment are unused.
    // Based on Internet searches, some other platforms use 8-byte-aligned strides.
    // I don't know how to query the runtime environment to tell what the default stride alignment is. Assume 4.

    int bytesPerPixel = bitsPerPixel / 8;

    if (0 != (bitsPerPixel % 8))
        bytesPerPixel++;

    const int AlignmentForStride = 4;

    return (((width * bytesPerPixel) + (AlignmentForStride - 1)) / AlignmentForStride) * AlignmentForStride;
} //StrideInBytes

HRESULT InitializeSinkWriter( IMFSinkWriter **ppWriter, DWORD *pStreamIndex, WCHAR * pwcOutput )
{
    *ppWriter = NULL;
    *pStreamIndex = NULL;

    IMFAttributes *pAttr = NULL;
    HRESULT hr = S_OK;

    if ( g_usegpu )
    {
        hr = MFCreateAttributes( &pAttr, 1 );

        // Hardware/GPU won't be used unless this is set. Overall runtime is >25% faster with the GPU

        if ( SUCCEEDED( hr ) )
            hr = pAttr->SetUINT32( MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE );

        // The app will run faster (if you have the RAM), but this can lead to tens of gigs of RAM usage.
        // If you turn this on, first increment the attribute count above.

        //if ( SUCCEEDED( hr ) )
        //    hr = pAttr->SetUINT32( MF_SINK_WRITER_DISABLE_THROTTLING, TRUE );
    }

    IMFSinkWriter * pSinkWriter = NULL;
    if ( SUCCEEDED( hr ) )
        hr = MFCreateSinkWriterFromURL( pwcOutput, NULL, pAttr, &pSinkWriter );

    IMFMediaType * pMediaTypeOut = NULL;   
    if ( SUCCEEDED( hr ) )
        hr = MFCreateMediaType( &pMediaTypeOut );   

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );     

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetGUID( MF_MT_SUBTYPE, VIDEO_ENCODING_FORMAT );   

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_AVG_BITRATE, g_video_bit_rate );

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );   

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeSize( pMediaTypeOut, MF_MT_FRAME_SIZE, g_width, g_height );

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeRatio( pMediaTypeOut, MF_MT_FRAME_RATE, VIDEO_FPS, 1 );   

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeRatio( pMediaTypeOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_DEFAULT_STRIDE, StrideInBytes( g_width, ALL_BPP ) );

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_FIXED_SIZE_SAMPLES, TRUE );

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE );

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeOut->SetUINT32( MF_MT_SAMPLE_SIZE, g_height * StrideInBytes( g_width, ALL_BPP ) );

    #if false
    if ( VIDEO_ENCODING_FORMAT == MFVideoFormat_H265 )
    {
        if ( SUCCEEDED( hr ) )
        {
            hr = pMediaTypeOut->SetUINT32( MF_MT_VIDEO_PROFILE, eAVEncH265VProfile_Main_420_8 );
            hr = pMediaTypeOut->SetUINT32( MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8 );

            if ( SUCCEEDED( hr ) )
                hr = pMediaTypeOut->SetUINT32( MF_MT_MPEG2_LEVEL, eAVEncH265VLevel1 );
        }
    }
    #endif

    DWORD streamIndex = 0;
    if ( SUCCEEDED( hr ) )
        hr = pSinkWriter->AddStream( pMediaTypeOut, &streamIndex );

    IMFMediaType * pMediaTypeIn = NULL;   
    if ( SUCCEEDED( hr ) )
        hr = MFCreateMediaType( &pMediaTypeIn );   

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeIn->SetGUID( MF_MT_MAJOR_TYPE, MFMediaType_Video );   

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeIn->SetGUID( MF_MT_SUBTYPE, VIDEO_INPUT_FORMAT );     

    if ( SUCCEEDED( hr ) )
        hr = pMediaTypeIn->SetUINT32( MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive );   

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeSize( pMediaTypeIn, MF_MT_FRAME_SIZE, g_width, g_height );

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeRatio( pMediaTypeIn, MF_MT_FRAME_RATE, VIDEO_FPS, 1 );   

    if ( SUCCEEDED( hr ) )
        hr = MFSetAttributeRatio( pMediaTypeIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1 );

    if ( SUCCEEDED( hr ) )
        hr = pSinkWriter->SetInputMediaType( streamIndex, pMediaTypeIn, NULL );   

    // Tell the sink writer to start accepting data.

    if ( SUCCEEDED( hr ) )
        hr = pSinkWriter->BeginWriting();

    if ( SUCCEEDED( hr ) )
    {
        *ppWriter = pSinkWriter;
        (*ppWriter)->AddRef();
        *pStreamIndex = streamIndex;
    }

    SafeRelease( &pAttr );
    SafeRelease( &pSinkWriter );
    SafeRelease( &pMediaTypeOut );
    SafeRelease( &pMediaTypeIn );
    return hr;
} //InitializeSinkWriter

HRESULT WriteFrame( IMFSinkWriter *pWriter, DWORD streamIndex, const LONGLONG& rtStart, LONGLONG & duration, byte * pFrame )
{
    int stride = StrideInBytes( g_width, ALL_BPP );
    const DWORD cbBuffer = stride * g_height;

    // Create a new memory buffer.

    IMFMediaBuffer *pBuffer = NULL;
    HRESULT hr = MFCreateMemoryBuffer( cbBuffer, &pBuffer ) ;
                             
    // Lock the buffer and copy the video frame to the buffer.

    BYTE *pData = NULL;
    if ( SUCCEEDED( hr ) )
        hr = pBuffer->Lock( &pData, NULL, NULL );

    if ( SUCCEEDED( hr ) )
        hr = MFCopyImage( pData, stride, pFrame, stride, g_width * 3, g_height );

    if ( pBuffer )
        pBuffer->Unlock();

    // Set the data length of the buffer.

    if ( SUCCEEDED( hr ) )
        hr = pBuffer->SetCurrentLength( cbBuffer );

    // Create a media sample and add the buffer to the sample.

    IMFSample *pSample = NULL;

    if ( SUCCEEDED( hr ) )
        hr = MFCreateSample( &pSample );

    if ( SUCCEEDED( hr ) )
        hr = pSample->AddBuffer( pBuffer );

    // Set the time stamp and the duration.

    if ( SUCCEEDED( hr ) )
        hr = pSample->SetSampleTime( rtStart );

    if ( SUCCEEDED( hr ) )
        hr = pSample->SetSampleDuration( duration );

    // Send the sample to the Sink Writer.

    if ( SUCCEEDED( hr ) )
        hr = pWriter->WriteSample( streamIndex, pSample );

    if ( SUCCEEDED( hr ) )
        hr = pWriter->NotifyEndOfSegment( streamIndex );

    SafeRelease( &pSample );
    SafeRelease( &pBuffer );
    return hr;
} //WriteFrame

static byte ** g_aBitFrames = 0;
static int g_animationFrames = 0;

bool AllocateTransitionFrames( int animationFrames, int bytesPerFrame, int stride )
{
    static bool allocatedyet = false;

    if ( allocatedyet )
        return true;

    allocatedyet = true;

    g_animationFrames = animationFrames;
    g_aBitFrames = new byte * [ animationFrames ];

    for ( int i = 0; i < animationFrames; i++ )
        g_aBitFrames[ i ] = new byte[ bytesPerFrame ];

    return false;
} //AlocateTransitionFrames

void FreeTransitionFrames()
{
    if ( 0 != g_aBitFrames )
    {
        for ( int i = 0; i < g_animationFrames; i++ )
            delete [] g_aBitFrames[ i ];

        delete g_aBitFrames;
    }
} //FreeAnimationFrames

HRESULT WriteTransitionFrame( IMFSinkWriter *pWriter, DWORD streamIndex, const LONGLONG& rtStart, LONGLONG & duration, byte * pFrame, int transition, int effect_ms )
{
    // rtStart and duration are in video units -- 10000 per MS

    if ( 0 == transition )
        return WriteFrame( pWriter, streamIndex, rtStart, duration, pFrame );

    float videoFrameTimeMS = 1000.0f / ( (float) VIDEO_FPS / 1.0f );
    int animationIntervalIS = (int) videoFrameTimeMS;
    int animationFrames = (int) ( (float) effect_ms / videoFrameTimeMS );

    int stride = StrideInBytes( g_width, ALL_BPP );
    int bytesPerFrame = g_height * stride;

    AllocateTransitionFrames( animationFrames, bytesPerFrame, stride );

    LONGLONG animationDuration = effect_ms * VIDEO_UNITS_PER_MS;
    LONGLONG animationDurationPerFrame = animationDuration / animationFrames;
    LONGLONG currentTime = rtStart;
    LONGLONG mainFrameDuration = duration - ( animationDuration * (LONGLONG) 2 );

    unique_ptr<Bitmap> sourceBitmap( new Bitmap( g_width, g_height, stride, PixelFormat24bppRGB, pFrame ) );

    if ( 1 == transition )
    {
        parallel_for( 0, animationFrames, [&] (int i)
        {
            float opacity = (float) ( i + 0.1f ) / (float) animationFrames;
            byte *p = g_aBitFrames[ i ];
    
            for ( int y = 0; y < g_height; y++ )
            {
                int row = y * stride;
                byte * prow = p + row;
                byte * prowFrame = pFrame + row;
                byte * prowEnd = prow + ( ALL_BYTESPP * g_width );
    
                do
                {
                    *prow++ = (byte) ( ( float) ( *prowFrame++ ) * opacity );
                    *prow++ = (byte) ( ( float) ( *prowFrame++ ) * opacity );
                    *prow++ = (byte) ( ( float) ( *prowFrame++ ) * opacity );
                } while ( prow < prowEnd );
            }
        } );
    }
    else if ( 2 == transition )
    {
        parallel_for( 0, animationFrames, [&] (int i)
        {
            float opacity = 1.0f - (float) i / (float) animationFrames;
            byte *p = g_aBitFrames[ i ];
    
            for ( int y = 0; y < g_height; y++ )
            {
                int row = y * stride;
                byte * prow = p + row;
                byte * prowFrame = pFrame + row;
                byte * prowEnd = prow + ( ALL_BYTESPP * g_width );
    
                do
                {
                    *prow++ = (byte) ( ( float) ( *prowFrame ) + ( ( 255 - *prowFrame++ ) * opacity ) );
                    *prow++ = (byte) ( ( float) ( *prowFrame ) + ( ( 255 - *prowFrame++ ) * opacity ) );
                    *prow++ = (byte) ( ( float) ( *prowFrame ) + ( ( 255 - *prowFrame++ ) * opacity ) );
                } while ( prow < prowEnd );
            }
        } );
    }

    HRESULT hr = S_OK;

    for ( int i = 0; SUCCEEDED( hr ) && ( i < animationFrames ); i++ )
    {
        hr = WriteFrame( pWriter, streamIndex, currentTime, animationDurationPerFrame, g_aBitFrames[ i ] );
        currentTime += animationDurationPerFrame;
    }

    if ( SUCCEEDED( hr ) )
    {
        hr = WriteFrame( pWriter, streamIndex, currentTime, mainFrameDuration, pFrame );
        currentTime += mainFrameDuration;
    }

    if ( SUCCEEDED( hr ) && ( 1 == transition ) || ( 2 == transition) )
    {
        for ( int f = animationFrames - 1; SUCCEEDED( hr ) && ( f >= 0 ); f-- )
        {
            hr = WriteFrame( pWriter, streamIndex, currentTime, animationDurationPerFrame, g_aBitFrames[ f ] );
            currentTime += animationDurationPerFrame;
        }
    }

    return hr;
} //WriteTransitionFrame

void ComputeEventualSize( int & targetw, int & targeth, Bitmap & frame, Bitmap & b, bool invertWH )
{
    int w = frame.GetWidth();
    int h = frame.GetHeight();

    int bw = b.GetWidth();
    int bh = b.GetHeight();

    if ( invertWH )
    {
        int temp = bw;
        bw = bh;
        bh = temp;
    }

    targetw = w;
    targeth = h;

    // Fit the bitmap such that when centered no data is lost, assuming black/fillcolor bars in places not used.

    if ( ( bw > w ) || ( bh > h ) )
    {
        if ( bw > w )
        {
            int scaledh = (int) ( ( (double) w / (double) bw ) * (double) bh );

            if ( scaledh > h )
            {
                int scaledw = (int) ( ( (double) h / (double) bh ) * (double) bw );

                targetw = scaledw;
                targeth = h;
            }
            else
            {
                targetw = w;
                targeth = scaledh;
            }
        }
        else
        {
            int scaledw = (int) ( ( (double) h / (double) bh ) * (double) bw );

            if ( scaledw > w )
            {
                int scaledh = (int) ( ( (double) w / (double) bw ) * (double) bh );

                targeth = scaledh;
                targetw = w;
            }
            else
            {
                targeth = h;
                targetw = scaledw;
            }
        }
    }
    else
    {
        if ( ( w - bw ) > ( h - bh ) )
        {
            targeth = h;
            targetw = (int) ( ( (double) h / (double) bh ) * (double) bw );
        }
        else
        {
            targetw = w;
            targeth = (int) ( ( (double) w / (double) bw ) * (double) bh );
        }
    }

    if ( invertWH )
    {
        int temp = targetw;
        targetw = targeth;
        targeth = temp;
    }
} //ComputeEventualSize

#ifndef USE_WIC_FOR_OPEN

Bitmap * ResizeBitmap( Bitmap * pb, int targetW, int targetH )
{
    Rect rectT( 0, 0, targetW, targetH );
    Bitmap * newBitmap = new Bitmap( targetW, targetH, PixelFormat24bppRGB );
    unique_ptr<Graphics> g( Graphics::FromImage( newBitmap ) );

    // The default is InterpolationModeBilinear. It's not InterpolationModeDefault :)

    g->SetCompositingMode( CompositingMode::CompositingModeSourceCopy );
    g->SetCompositingQuality( CompositingQuality::CompositingQualityHighQuality );
    g->SetInterpolationMode( InterpolationMode::InterpolationModeHighQualityBicubic );
    g->SetPixelOffsetMode( PixelOffsetMode::PixelOffsetModeHighQuality );

    // This is very slow and must internally have a lock, as it blocks all other threads.
    // Much of the runtime of the app will be here (if not using WIC)

    g->DrawImage( pb, rectT, 0, 0, pb->GetWidth(), pb->GetHeight(), UnitPixel, NULL, NULL );

    return newBitmap;
} //ResizeBitmap

static int ExifRotateValue( Image & img )
{
    const int exifOrientationID = 0x112; //274

    int size = img.GetPropertyItemSize( exifOrientationID );
    vector<byte> bytes( size );
    int val = -1;
    PropertyItem * pItem = (PropertyItem *) bytes.data();
    Status status = img.GetPropertyItem( exifOrientationID, size, pItem );

    if ( Ok == status )
        val = * (USHORT *) pItem->value;

    return val;
} //ExifRotateValue

// For the shirts data, this runs in 3.5s as opposed to 4.3s for the built-in rotate
// There is an 8x performance difference optimizing for cache hits on Before instead of After.

Bitmap * Rotate90( Bitmap & before )
{
    Bitmap * after = new Bitmap( before.GetHeight(), before.GetWidth(), PixelFormat24bppRGB );

    Rect rectAfter( 0, 0, after->GetWidth(), after->GetHeight() );
    BitmapData bdAfter;
    after->LockBits( &rectAfter, ImageLockModeWrite, PixelFormat24bppRGB, &bdAfter );
    int strideAfter = abs( bdAfter.Stride );

    if ( strideAfter != StrideInBytes( after->GetWidth(), ALL_BPP ) )
        printf( "stride After not expected\n" );

    Rect rectBefore( 0, 0, before.GetWidth(), before.GetHeight() );
    BitmapData bdBefore;

    // The native pixel format is 24bppRGB. Reading anything else is much slower.

    before.LockBits( &rectBefore, ImageLockModeRead, PixelFormat24bppRGB, &bdBefore );
    int strideBefore = abs( bdBefore.Stride );

    if ( strideBefore != StrideInBytes( before.GetWidth(), ALL_BPP ) )
        printf( "stride Before not expected\n" );

    byte * pixelAfterBase = (byte *) bdAfter.Scan0 + strideAfter;
    byte * pixelBeforeBase = (byte *) bdBefore.Scan0;
    int beforeHeight = before.GetHeight();

    //for ( int y = 0; y < beforeHeight; y++ )
    parallel_for( 0, int( beforeHeight ), [&] ( int y )
    {
        byte * pixelBefore = pixelBeforeBase + ( y * strideBefore );
        byte * pixelAfter = pixelAfterBase - ( ( y + 1 ) * ALL_BYTESPP );
        byte * pixelBeforeEnd = pixelBefore + strideBefore;

        do
        {
            memcpy( pixelAfter, pixelBefore, ALL_BYTESPP );
            pixelAfter += strideAfter;
            pixelBefore += ALL_BYTESPP;
        } while ( pixelBefore < pixelBeforeEnd );
    } );

    before.UnlockBits( &bdBefore );
    after->UnlockBits( &bdAfter );

    return after;
} //Rotate90

static void ExifRotate( Image & img, int val, bool flipY )
{
    if ( 0 != val || flipY )
    {
        RotateFlipType rot = RotateNoneFlipNone;
    
        if ( val == 3 || val == 4 )
            rot = Rotate180FlipNone;
        else if ( val == 5 || val == 6 )
            rot = Rotate90FlipNone;
        else if ( val == 7 || val == 8 )
            rot = Rotate270FlipNone;
    
        if ( val == 2 || val == 4 || val == 5 || val == 7 )
            rot = (RotateFlipType) ( (int) rot | (int) RotateNoneFlipX );

        if ( flipY )
            rot = (RotateFlipType) ( (int) rot | (int) RotateNoneFlipY );

        if ( rot != RotateNoneFlipNone )
            img.RotateFlip( rot );
    }
} //ExifRotate

#endif // USE_WIC_FOR_OPEN

void DrawCaption( Bitmap & frame, const WCHAR * pwcPath )
{
    vector<WCHAR> caption( 1 + wcslen( pwcPath ) );
    wcscpy( caption.data(), pwcPath );
    WCHAR * dot = wcsrchr( caption.data(), L'.' );
    if ( dot )
        *dot = 0;

    WCHAR * slash = wcsrchr( caption.data(), L'\\' );
    if ( slash )
        wcscpy( caption.data(), slash + 1 );

    for ( WCHAR *p = caption.data(); *p; p++ )
        if ( L'-' == *p )
            *p = ' ';

    FontFamily fontFamily( L"Arial" );
    Font font( &fontFamily, 12, FontStyleBold, UnitPoint );

    StringFormat stringFormat;
    stringFormat.SetAlignment( StringAlignmentCenter );
    stringFormat.SetLineAlignment( StringAlignmentCenter );

    RectF rect;
    Unit unit;
    frame.GetBounds( &rect, &unit );

    RectF rectLower( rect.GetLeft(), rect.GetBottom() * 3.0 / 4.0, rect.GetRight(), rect.GetBottom() / 4.0 );

    Graphics graphics( & frame );
    graphics.SetSmoothingMode( SmoothingModeAntiAlias );
    graphics.SetInterpolationMode( InterpolationModeHighQualityBicubic );

    GraphicsPath path;
    path.AddString( caption.data(), wcslen( caption.data() ), &fontFamily, FontStyleRegular, 12, rectLower, &stringFormat );
    Pen pen( Color( 255, 255, 255 ), 4 );
    pen.SetLineJoin( LineJoinRound );
    graphics.DrawPath( &pen, &path);
    SolidBrush brush( Color( 0, 0, 0 ) );
    graphics.FillPath( &brush, &path );

//    SolidBrush solidBrush( Color( 255, 255, 127, 127 ) );
//    graphics.DrawString( caption.data(), -1, &font, rectLower, &stringFormat, &solidBrush );
} //DrawCaption

void FitBitmapInFrame( Bitmap & frame, Bitmap & b )
{
    int w = frame.GetWidth();
    int h = frame.GetHeight();

    int bw = b.GetWidth();
    int bh = b.GetHeight();

    if ( bw == w && bh == h )
    {
        Rect rect( 0, 0, w, h );
        BitmapData bdFrame;
        frame.LockBits( &rect, ImageLockModeWrite, PixelFormat24bppRGB, &bdFrame );

        BitmapData bdb;
        b.LockBits( &rect, ImageLockModeRead, PixelFormat24bppRGB, &bdb );

        int stride = abs( bdb.Stride );
        int bytes = h * stride;

        memcpy( bdFrame.Scan0, bdb.Scan0, bytes );

        frame.UnlockBits( &bdFrame );
        b.UnlockBits( &bdb );
    }
    else
    {
        int targetw = w;
        int targeth = h;
    
        ComputeEventualSize( targetw, targeth, frame, b, false );
    
        //printf( "w %d, h %d, bw %d, bh %d, targetw %d, targeth %d\n", w, h, bw, bh, targetw, targeth );
    
        unique_ptr<Graphics> g( Graphics::FromImage( &frame ) );
    
        int dstX = ( w - targetw ) / 2;
        int dstY = ( h - targeth ) / 2;
    
        // Fill if the image doesn't fit the frame exactly
    
        if ( 0 != dstX || 0 != dstY )
        {
            Rect rectFill( 0, 0, w, h );
            SolidBrush brushFill( Color( g_fill_red, g_fill_green, g_fill_blue ) );
            g->FillRectangle( &brushFill, rectFill );
        }

        // Unless upscaling, one or the other dimension will match exactly. So it's basically a blt, not a resize
    
        Rect rectT( dstX, dstY, targetw, targeth );
    
        g->SetCompositingMode( CompositingMode::CompositingModeSourceCopy );
        g->SetCompositingQuality( CompositingQuality::CompositingQualityHighQuality );
        g->SetInterpolationMode( InterpolationMode::InterpolationModeHighQualityBicubic );
        g->SetPixelOffsetMode( PixelOffsetMode::PixelOffsetModeHighQuality );

        g->DrawImage( &b, rectT, 0, 0, bw, bh, UnitPixel, NULL, NULL );

        // this works too, but is no faster. I'm not using in case nonscaled images are passed in the future
        // g->DrawImage( &b, dstX, dstY );
    }
} //FitBitmapInFrame

void FlipY( Bitmap & b )
{
    Rect rect( 0, 0, b.GetWidth(), b.GetHeight() );
    BitmapData bd;
    b.LockBits( &rect, ImageLockModeWrite, PixelFormat24bppRGB, &bd );
    int stride = abs( bd.Stride );

    if ( stride != StrideInBytes( b.GetWidth(), ALL_BPP ) )
        printf( "stride in FlipY not expected\n" );

    byte * p = (byte *) bd.Scan0;
    int height = b.GetHeight();
    int half = height / 2;
    int parallel = __min( 4, half );
    int ineach = half / parallel;

    parallel_for( 0, parallel, [&] (int t)
    {
        vector<byte> row( stride );
        int top = t * ineach;
        int beyondtop = ( t == ( parallel - 1 ) ) ? half : ( top + ineach );
        int bottom = ( height - 1 ) - top;
        byte * ptop = p + ( top * stride );
        byte * pbottom = p + ( bottom * stride );

        while ( top < beyondtop )
        {
            memcpy( row.data(), ptop, stride );
            memcpy( ptop, pbottom, stride );
            memcpy( pbottom, row.data(), stride );

            ptop += stride;
            pbottom -= stride;
            top++;
            bottom--;
        }
    } );

    b.UnlockBits( &bd );
} //FlipY

extern "C" int __cdecl wmain( int argc, WCHAR * argv[] )
{
    CPerfTime perfApp;
    LONGLONG startTime = perfApp.TimeNow();

    _set_se_translator([](unsigned int u, EXCEPTION_POINTERS *pExp)
    {
        printf( "translating exception %x\n", u );
        std::string error = "SE Exception: ";
        switch (u)
        {
            case 0xC0000005:
                error += "Access Violation";
                break;
            default:
                char result[11];
                sprintf_s(result, 11, "0x%08X", u);
                error += result;
        };

        printf( "throwing std::exception\n" );
    
        throw std::exception(error.c_str());
    });

    tracer.Enable( false, L"cv.txt", true );

    int iArg = 1;
    g_input_spec[ 0 ] = 0;
    g_input_text_file[ 0 ] = 0;
    g_output_file[ 0 ] = 0;
    WCHAR sortOrder = 'r';
    bool preserveFileOrder = false;

    while ( iArg < argc )
    {
        const WCHAR * pwcArg = argv[iArg];
        WCHAR a0 = pwcArg[0];

        if ( ( L'-' == a0 ) ||
             ( L'/' == a0 ) )
        {
           WCHAR a1 = towlower( pwcArg[1] );

           if ( L'b' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_video_bit_rate = _wtoi( pwcArg + 3 );

               if ( 0 == g_video_bit_rate )
               {
                   printf( "invalid bitrate\n\n" );
                   Usage();
               }
           }
           else if ( L'c' == a1 )
           {
               if ( 0 != pwcArg[2] )
                   Usage();

               g_captions = true;
           }
           else if ( L'd' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_ms_delay = _wtoi( pwcArg + 3 );

               if ( 0 == g_ms_delay )
               {
                   printf( "invalid delay\n\n" );
                   Usage();
               }
           }
           else if ( L'e' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_ms_transition_effect = _wtoi( pwcArg + 3 );

               if ( 0 == g_ms_transition_effect )
               {
                   printf( "invalid transition effect delay\n\n" );
                   Usage();
               }
           }
           else if ( L'f' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               int colors = 0;
               int parsed = swscanf_s( pwcArg + 3, L"%x", & colors);

               if ( 0 == parsed )
               {
                   printf( "can't parse fill color\n\n" );
                   Usage();
               }

               g_fill_red = colors & 0xff;
               g_fill_green = ( colors >> 8 ) & 0xff;
               g_fill_blue = ( colors >> 16 ) & 0xff;
           }
           else if ( L'g' == a1 )
           {
               if ( 0 != pwcArg[2] )
                   Usage();

               g_usegpu = false;
           }
           else if ( L'h' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_height = _wtoi( pwcArg + 3 );

               if ( 0 == g_height )
               {
                   printf( "invalid height\n\n" );
                   Usage();
               }
           }
           else if ( L'i' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               wcscpy( g_input_text_file, pwcArg + 3 );
           }
           else if ( L'w' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_width = _wtoi( pwcArg + 3 );

               if ( 0 == g_width )
               {
                   printf( "invalid width\n\n" );
                   Usage();
               }
           }
           else if ( L'o' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               int c = wcslen( pwcArg + 3 );

               if ( 0 == c )
               {
                   printf( "invalid output filename\n\n" );
                   Usage();
               }

               wcscpy( g_output_file, pwcArg + 3 );
           }
           else if ( L'p' == a1 )
           {
               if ( L':' != pwcArg[2] )
                   Usage();

               g_parallelism = _wtoi( pwcArg + 3 );

               if ( ( g_parallelism < 1 ) || ( g_parallelism > 16 ) )
               {
                   printf( "invalid parallelism\n\n" );
                   Usage();
               }
           }
           else if ( L'r' == a1 )
           {
               if ( 0 != pwcArg[2] )
                   Usage();

               g_recurse = true;
           }
           else if ( L's' == a1 )
           {
               if ( 0 == pwcArg[2] || 0 == pwcArg[3] )
                   Usage();

               sortOrder = pwcArg[ 3 ];
               WCHAR lorder = tolower( sortOrder );

               if ( 'w' != lorder && 'c' != lorder && 'u' != lorder && 'p' != lorder && 'r' != lorder )
               {
                   printf( "invalid sort order\n\n" );
                   Usage();
               }
           }
           else if ( L't' == a1 )
           {
               if ( L':' != pwcArg[2] )
               {
                   printf( "no transition type specified with /t. This is required\n" );
                   Usage();
               }

               g_transition = _wtoi( pwcArg + 3 );

               if ( ( g_transition < 1 ) || ( g_transition > 2 ) )
               {
                   printf( "invalid transition\n\n" );
                   Usage();
               }
           }
           else if ( L'z' == a1 )
           {
               if ( 0 != pwcArg[2] )
                   Usage();

               g_stats = true;
           }
           else
           {
               printf( "unrecognized argument %c\n", a1 );
               Usage();
           }
        }
        else
        {
            if ( 0 != g_input_spec[ 0 ] )
                Usage();

            wcscpy( g_input_spec, argv[ iArg ] );
        }

       iArg++;
    }

    if ( ( 0 != g_transition ) && ( g_ms_transition_effect * 2 ) >= g_ms_delay )
    {
        printf( "The transition effect time must be less than half the transition delay\n" );
        Usage();
    }

    if ( 0 == g_input_spec[0] && 0 == g_input_text_file[0] )
    {
        printf( "no input specified\n" );
        Usage();
    }

    if ( 0 != g_input_spec[0] && 0 != g_input_text_file[0] )
    {
        printf( "Only one of [input] and /i:[textfile] can be specified. Both were requested\n" );
        Usage();
    }

    if ( 0 == g_output_file[ 0 ] )
    {
        printf( "no output file specified\n\n" );
        Usage();
    }

    CPathArray paths;

    if ( 0 != g_input_spec[0] )
    {
        static WCHAR awcPath[ MAX_PATH + 1 ] = {0};
        static WCHAR awcSpec[ MAX_PATH + 1 ] = {0};
    
        WCHAR * pwcSlash = wcsrchr( g_input_spec, L'\\' );
    
        if ( NULL == pwcSlash )
        {
            wcscpy( awcSpec, g_input_spec );
            _wfullpath( awcPath, L".\\", _countof( awcPath ) );
        }
        else
        {
            wcscpy( awcSpec, pwcSlash + 1 );
            *(pwcSlash + 1) = 0;
            _wfullpath( awcPath, g_input_spec, _countof( awcPath ) );
        }
    
        printf( "Path '%ws', File Specificaiton '%ws'\n", awcPath, awcSpec );

        CEnumFolder enumFolder( g_recurse, &paths, NULL, 0 );
        enumFolder.Enumerate( awcPath, awcSpec );
    }
    else
    {
        FILE * file = _wfopen( g_input_text_file, L"r" );
        if ( 0 == file )
        {
            printf( "can't open input text file %ws\n", g_input_text_file );
            Usage();
        }

        WCHAR awcLine[ MAX_PATH + 1 ];

        while ( fgetws( awcLine, sizeof awcLine / sizeof awcLine[0], file ) )
        {
            int c = wcslen( awcLine ) - 1;

            while ( ( c >= 0 ) && ( ( awcLine[c] == 0xd ) || ( awcLine[c] == 0xa ) ) )
            {
                awcLine[c] = 0;
                c--;
            }

            if ( 0 != awcLine[0] )
                paths.Add( awcLine );
        }

        fclose( file );
    }

    if ( 0 == paths.Count() )
    {
        printf( "no input files found\n\n" );
        Usage();
    }

    WCHAR lorder = tolower( sortOrder );
    if ( 'w' == lorder )
        paths.SortOnLastWrite();
    else if ( 'c' == lorder )
        paths.SortOnCreation();
    else if ( 'u' == lorder )
        paths.SortOnCapture();
    else if ( 'p' == lorder )
        paths.SortOnPath();
    else if ( 'r' == lorder )
        paths.Randomize();

    if ( lorder != sortOrder )
        paths.InvertSort();

    if ( 'r' != lorder )
        preserveFileOrder = true;

    printf( "%zd input files\n", paths.Count() );

    int frameStride = StrideInBytes( g_width, ALL_BPP );

    byte ** frame_batch = new byte * [ g_parallelism ];
    ZeroMemory( frame_batch, sizeof (byte *) * g_parallelism );

    Bitmap ** frame_bitmap_batch = new Bitmap * [ g_parallelism ];
    ZeroMemory( frame_bitmap_batch, sizeof (Bitmap *) * g_parallelism );

    HANDLE *frame_event_batch = new HANDLE[ g_parallelism ];
    ZeroMemory( frame_event_batch, sizeof( HANDLE ) * g_parallelism );

    LONGLONG totalLoadTime = 0;
    LONGLONG totalReadRotateTime = 0;
    LONGLONG totalResizeTime = 0;
    LONGLONG totalRotateTime = 0;
    LONGLONG totalFlipTime = 0;
    LONGLONG totalFitTime = 0;
    LONGLONG totalWaitTime = 0;
    LONGLONG totalFrameTime = 0;
    LONGLONG totalFinalizeTime = 0;

    try
    {
        HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED ); //APARTMENTTHREADED);
        if ( SUCCEEDED( hr ) )
        {
            hr = MFStartup( MF_VERSION );
            if ( SUCCEEDED( hr ) )
            {
                #ifdef USE_WIC_FOR_OPEN
                    CWic2Gdi wic2gdi;
                    if ( !wic2gdi.Ok() )
                    {
                        printf( "can't initialize WIC\n" );
                        exit( 1 );
                    }
                #endif

                IMFSinkWriter *pSinkWriter = NULL;
                DWORD stream;
    
                ULONG_PTR gdiplusToken = 0;
                hr = InitializeSinkWriter(&pSinkWriter, &stream, g_output_file );
                if ( SUCCEEDED( hr ) )
                {
                    GdiplusStartupInput si;
                    GdiplusStartup( &gdiplusToken, &si, NULL );
    
                    for ( int i = 0; i < g_parallelism; i++ )
                    {
                        frame_batch[ i ] = new byte[ frameStride * g_height ];
                        frame_bitmap_batch[ i ] = new Bitmap( g_width, g_height, frameStride, PixelFormat24bppRGB, frame_batch[ i ] );
                        frame_event_batch[ i ] = CreateEvent( NULL, FALSE, FALSE, NULL );
                    }
    
                    LONGLONG duration = ( g_ms_delay * 1000 * 10 );
                    int iframe = 0;
                    while ( iframe < paths.Count() )
                    {
                        int batchsize = __min( g_parallelism, ( paths.Count() - iframe ) );
                        int batchBaseFrame = iframe;
    
                        parallel_for ( int(0), int(batchsize), [&]( int item )
                        {
                            try
                            {
                                // Give priority to threads encoding finished frames, so they don't sit around consuming RAM
                                // Let the first of the batch finish first, so other frames wait less

                                if ( preserveFileOrder && ( 0 == item ) )
                                    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL );
                                else
                                    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_LOWEST );

                                CPerfTime perfLoop;

                                #ifdef USE_WIC_FOR_OPEN // loading via WIC is much faster because scaling is done during decompression
                                    int aWidth, aHeight;
                                    int targetW = frame_bitmap_batch[ item ]->GetWidth();
                                    int targetH = frame_bitmap_batch[ item ]->GetHeight();
                                    byte * pbuffer = 0;
                                    unique_ptr<Bitmap> bitmap( wic2gdi.GDIPBitmapFromWIC( paths.Get( batchBaseFrame + item ), 0, &pbuffer,
                                                                                          targetW, targetH, &aWidth, &aHeight, PixelFormat24bppRGB ) );
                                    unique_ptr<byte> bitmap_buffer( pbuffer );
                                    perfLoop.CumulateSince( totalLoadTime );
    
                                    if ( NULL == bitmap.get() || 0 == bitmap->GetWidth() )
                                    {
                                        printf( "error, can't open file %ws\n", paths.Get( batchBaseFrame + item ) );
                                        exit( 1 );
                                    }
                                #else
                                    unique_ptr<Bitmap> bitmap( new Bitmap( paths.Get( batchBaseFrame + item ), FALSE ) );
                                    perfLoop.CumulateSince( totalLoadTime );
    
                                    if ( NULL == bitmap.get() || 0 == bitmap->GetWidth() )
                                    {
                                        printf( "error, can't open file %ws\n", paths.Get( batchBaseFrame + item ) );
                                        exit( 1 );
                                    }
            
                                    int val = ExifRotateValue( *bitmap );
                                    bool invertWH = ( val >= 5 && val <= 8 );
                                    perfLoop.CumulateSince( totalReadRotateTime );
            
                                    int eventualW, eventualH;
                                    ComputeEventualSize( eventualW, eventualH, *frame_bitmap_batch[ item ], *bitmap, invertWH );
                                    bitmap.reset( ResizeBitmap( bitmap.get(), eventualW, eventualH ) );
    
                                    perfLoop.CumulateSince( totalResizeTime );
            
                                    if ( 6 == val )
                                        bitmap.reset( Rotate90( *bitmap ) ); // 4.7 times faster than ExifRotate!
                                    else
                                        ExifRotate( *bitmap, val, FALSE );
    
                                    perfLoop.CumulateSince( totalRotateTime );
                                #endif

                                FitBitmapInFrame( *frame_bitmap_batch[ item ], *bitmap );
                                perfLoop.CumulateSince( totalFitTime );

                                if ( g_captions )
                                    DrawCaption( *frame_bitmap_batch[ item ], paths.Get( batchBaseFrame + item ) );

                                // FlipY is 15x faster than bitmap->RotateFlip( RotateNoneFlipY );

                                FlipY( *frame_bitmap_batch[ item ] );
                                perfLoop.CumulateSince( totalFlipTime );
        
                                if ( preserveFileOrder && ( 0 != item ) )
                                    WaitForSingleObject( frame_event_batch[ item - 1 ], INFINITE );

                                // In the preserveFileOrder case, taking this lock is redundant because events make the code single-threaded. But it doesn't hurt.

                                lock_guard<mutex> lock( g_mtx );

                                perfLoop.CumulateSince( totalWaitTime ); // total of waiting for an (optinal) event and the lock

                                hr = WriteTransitionFrame( pSinkWriter, stream, (LONGLONG) iframe * (LONGLONG) duration, duration, frame_batch[ item ], g_transition, g_ms_transition_effect );
                                if (FAILED(hr))
                                {
                                    printf( "can't write frame: %x\n", hr );
                                    exit( -2 );
                                }
    
                                iframe++;
    
                                if ( 0 == ( iframe % 50 ) )
                                    printf( "\n%d files completed", iframe );
                                else
                                    printf( "." );
    
                                perfLoop.CumulateSince( totalFrameTime );

                                if ( preserveFileOrder && ( item != ( batchsize - 1 ) ) )
                                    SetEvent( frame_event_batch[ item ] );
                            }
                            catch( std::exception & ex )
                            {
                                printf( "caught exception processing an image: %s\n, exiting", ex.what() );
                                exit( -1 );
                            }
                            catch( ... )
                            {
                                printf( "caught a generic exception processing an image; exiting\n" );
                                exit( -1 );
                            }
                        });
                    }
                }
                else
                {
                    printf( "error %x initializing sink writer\n", hr );
                }

                CPerfTime finalizeTimer;

                if ( SUCCEEDED( hr ) )
                {
                    printf( "\ncalling finalize() to finish compressing and writing the video...\n" );
                    hr = pSinkWriter->Finalize();
                }

                finalizeTimer.CumulateSince( totalFinalizeTime );
    
                SafeRelease( &pSinkWriter );

                // Free resources

                {
                    if ( 0 != frame_batch )
                    {
                        for ( int i = 0; i < g_parallelism; i++ )
                        {
                            delete frame_batch[ i ];
                            frame_batch[ i ] = NULL;
                        }
                
                        delete frame_batch;
                    }
                
                    if ( 0 != frame_bitmap_batch )
                    {
                        for ( int i = 0; i < g_parallelism; i++ )
                        {
                            delete frame_bitmap_batch[ i ];
                            frame_bitmap_batch[ i ] = NULL;
                        }
                
                        delete frame_bitmap_batch;
                    }
                
                    if ( 0 != frame_event_batch )
                    {
                        for ( int i = 0; i < g_parallelism; i++ )
                        {
                            if ( 0 != frame_event_batch[ i ] )
                            {
                                CloseHandle( frame_event_batch[ i ] );
                                frame_event_batch[ i ] = 0;
                            }
                        }
                
                        delete frame_event_batch;
                    }

                    FreeTransitionFrames();
                }
    
                // GdiplusShutdown may not be needed; I think MFShutdown() does this. 
    
                GdiplusShutdown( gdiplusToken ); 
                MFShutdown();
            }
    
            CoUninitialize();
        }
    }
    catch( std::exception & ex )
    {
        printf( "caught exception %s in cv.exe\n", ex.what() );
        exit( -1 );
    }
    catch( ... )
    {
        printf( "caught a generic exception in cv.exe; exiting\n" );
        exit( -1 );
    }

    printf( "\nVideo creation complete: %ws\n", g_output_file );

    if ( g_stats )
    {
        printf( "\n" );

        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof pmc;
    
        if ( GetProcessMemoryInfo( GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS) &pmc, sizeof PROCESS_MEMORY_COUNTERS_EX ) )
        {
            printf( "peak working set  %14ws\n", perfApp.RenderLL( pmc.PeakWorkingSetSize ) );
            printf( "working set       %14ws\n", perfApp.RenderLL( pmc.WorkingSetSize ) );
            printf( "\n" );
        }
    
        LONGLONG elapsed = 0;
        perfApp.CumulateSince( elapsed );
        printf( "total elapsed    %15ws\n", perfApp.RenderDurationInMS( elapsed ) );
        printf( "  load           %15ws\n", perfApp.RenderDurationInMS( totalLoadTime ) );
        if ( 0 != totalReadRotateTime )
            printf( "  readrot        %15ws\n", perfApp.RenderDurationInMS( totalReadRotateTime ) );
        if ( 0 != totalResizeTime )
            printf( "  resize         %15ws\n", perfApp.RenderDurationInMS( totalResizeTime ) );
        if ( 0 != totalRotateTime )
            printf( "  rotate         %15ws\n", perfApp.RenderDurationInMS( totalRotateTime ) );
        printf( "  flip           %15ws\n", perfApp.RenderDurationInMS( totalFlipTime ) );
        printf( "  fit            %15ws\n", perfApp.RenderDurationInMS( totalFitTime ) );
        printf( "  wait           %15ws\n", perfApp.RenderDurationInMS( totalWaitTime ) );
        printf( "  frame          %15ws\n", perfApp.RenderDurationInMS( totalFrameTime ) );
        printf( "  finalize       %15ws\n", perfApp.RenderDurationInMS( totalFinalizeTime ) );
        printf( "  TOTAL          %15ws\n", perfApp.RenderDurationInMS( totalLoadTime + totalReadRotateTime + totalResizeTime + totalRotateTime +
                                                                        totalFlipTime + +totalFlipTime + totalFitTime + totalWaitTime +
                                                                        totalFrameTime + totalFinalizeTime ) );
        printf( "\n" );
    
        FILETIME creationFT, exitFT, kernelFT, userFT;
        GetProcessTimes( GetCurrentProcess(), &creationFT, &exitFT, &kernelFT, &userFT );
    
        ULARGE_INTEGER ullK, ullU;
        ullK.HighPart = kernelFT.dwHighDateTime;
        ullK.LowPart = kernelFT.dwLowDateTime;
    
        ullU.HighPart = userFT.dwHighDateTime;
        ullU.LowPart = userFT.dwLowDateTime;
    
        printf( "kernel CPU        %14ws\n", perfApp.RenderDurationInMS( ullK.QuadPart ) );
        printf( "user CPU          %14ws\n", perfApp.RenderDurationInMS( ullU.QuadPart ) );
        printf( "total CPU         %14ws\n", perfApp.RenderDurationInMS( ullU.QuadPart + ullK.QuadPart ) );
        printf( "avg. cores used   %14.2lf%\n", (double) ( ullU.QuadPart + ullK.QuadPart ) / (double) elapsed );
    }
} //wmain
