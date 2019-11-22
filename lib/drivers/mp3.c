#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/rom.h"
#include "sound.h"
#include "mp3.h"
#include "minimp3.h"
// #include "minimp3_ex.h"

//*****************************************************************************
//
// Audio buffer size and flags definitions  for indicating state of the buffer.
//
//*****************************************************************************
#define AUDIO_BUFFER_SIZE       4096

// UDMA PING-PONG Buffer
#define BUFFER_BOTTOM_EMPTY 0x00000001
#define BUFFER_TOP_EMPTY    0x00000002
#define BUFFER_PLAYING      0x00000004
//*****************************************************************************
//
// Allocate the audio clip buffer.
//
//*****************************************************************************
static unsigned char g_pucBuffer[AUDIO_BUFFER_SIZE];
// static short g_pucBuffer[AUDIO_BUFFER_SIZE];
static unsigned char *g_pucDataPtr;
static unsigned long g_ulMaxBufferSize;

//*****************************************************************************
//
// Holds the state of the audio buffer.
//
//*****************************************************************************
static volatile unsigned long g_ulFlags;

//*****************************************************************************
//
// Variables used to track playback position and time.
//
//*****************************************************************************
static unsigned long g_ulBytesPlayed;
static unsigned long g_ulBytesRemaining;

//*****************************************************************************
//
// mp3 specific variables
//
//*****************************************************************************
static mp3dec_t mp3d;
mp3dec_frame_info_t info;

//*****************************************************************************
//
// This function is called by the sound driver when a buffer has been
// played.  It marks the buffer (top half or bottom half) as free.
//
//*****************************************************************************
static void
BufferCallback(void *pvBuffer, unsigned long ulEvent)
{
    //
    // Handle buffer-free event
    //
    if(ulEvent & BUFFER_EVENT_FREE)
    {
        //
        // If pointing at the start of the buffer, then this is the first
        // half, so mark it as free.
        //
        if(pvBuffer == g_pucBuffer)
        {
            g_ulFlags |= BUFFER_BOTTOM_EMPTY;
        }

        //
        // Otherwise it must be the second half of the buffer that is free.
        //
        else
        {
            g_ulFlags |= BUFFER_TOP_EMPTY;
        }

        //
        // Update the byte count.
        //
        g_ulBytesPlayed += AUDIO_BUFFER_SIZE >> 1;
    }
}

void
Mp3PlayStart(unsigned char *pulAddress, const unsigned int len)
{
    //
    // Mark both buffers as empty.
    //
    g_ulFlags = BUFFER_BOTTOM_EMPTY | BUFFER_TOP_EMPTY;

    //
    // Indicate that the application is about to start playing.
    //
    g_ulFlags |= BUFFER_PLAYING;

    //
    // Set the data pointer to the start of the data
    //
    g_pucDataPtr = pulAddress;

    //
    // Set data length
    //
    g_ulBytesRemaining = len;

    //
    // mp3 init source
    //
    mp3dec_init(&mp3d);
    
    //
    // Enable the Class D amp.  It's turned off when idle to save power.
    //
    SoundClassDEn();

    //
    // Define input buffer size
    //

    g_ulMaxBufferSize = 4096;
}

//*****************************************************************************
//
// Read next block of data from audio clip.
//
// \param pucBuffer is a pointer to a buffer where the next block of data
// will be copied.
//
// This function reads the next block of data from the playing audio clip
// and stores it in the caller-supplied buffer.  If required the data will
// be converted in-place from 8-bit unsigned to 8-bit signed.
//
// \return The number of bytes read from the audio clip and stored in the
// buffer.
//
//*****************************************************************************

static unsigned long
PcmRead(unsigned char *pucBuffer)
{
    int i;

    unsigned long ulBytesToRead;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    //
    // Either read a half buffer or just the bytes remaining if we are at the
    // end of the file.
    //
    if(g_ulBytesRemaining < g_ulMaxBufferSize)
    {
        ulBytesToRead = g_ulBytesRemaining;
    }
    else
    {
        ulBytesToRead = g_ulMaxBufferSize;
    }

    
    mp3dec_decode_frame(&mp3d, g_pucDataPtr, ulBytesToRead, pcm, &info);
    g_pucDataPtr -= info.frame_bytes;

    //
    // Decrement the number of data bytes remaining to be read.
    //
    g_ulBytesRemaining -= ulBytesToRead;

    //
    // Update the global data pointer keeping track of the location in flash.
    //
    g_pucDataPtr += ulBytesToRead;

    
    // Copy data from the playing audio clip to the caller-supplied buffer.
    // This buffer will be in SRAM which is required in case the data needs
    // 8-bit sign conversion, and also so that the buffer can be handled by
    // uDMA.
    
    for(i = 0; i < info.frame_bytes; i++)
    {
        pucBuffer[i] = pcm[i];
    }

    return(info.frame_bytes);
}

tBoolean
Mp3PlayContinue()
{
    tBoolean bRetcode;
    unsigned long ulCount;

    //
    // Assume we're not finished until we learn otherwise.
    //
    bRetcode = false;

    //
    // Set a value that we can use to tell whether or not we processed any
    // new data.
    //
    ulCount = 0xFFFFFFFF;
    
    //
    // Must disable I2S interrupts during this time to prevent state problems.
    // Same is done in sound.c so it may be safe to ommit
    //
    ROM_IntDisable(INT_I2S0);


    //
    // If the refill flag gets cleared then fill the requested side of the
    // buffer.
    //
    if(g_ulFlags & BUFFER_BOTTOM_EMPTY)
    {
        //
        // Read out the next buffer worth of data.
        //
        ulCount = PcmRead(g_pucBuffer);

        //
        // Start the playback for a new buffer.
        //
        SoundBufferPlay(g_pucBuffer, ulCount, BufferCallback);

        //
        // Bottom half of the buffer is now not empty.
        //
        g_ulFlags &= ~BUFFER_BOTTOM_EMPTY;
    }

    if(g_ulFlags & BUFFER_TOP_EMPTY)
    {
        //
        // Read out the next buffer worth of data.
        //
        ulCount = PcmRead(&g_pucBuffer[AUDIO_BUFFER_SIZE >> 1]);

        //
        // Start the playback for a new buffer.
        //
        SoundBufferPlay(&g_pucBuffer[AUDIO_BUFFER_SIZE >> 1],
                        ulCount, BufferCallback);

        //
        // Top half of the buffer is now not empty.
        //
        g_ulFlags &= ~BUFFER_TOP_EMPTY;
    }

    //
    // Audio playback is done once the count is below a full buffer.
    //
    if((ulCount < g_ulMaxBufferSize) || (g_ulBytesRemaining == 0))
    {
        //
        // No longer playing audio.
        //
        g_ulFlags &= ~BUFFER_PLAYING;

        //
        // Wait for the buffer to empty.
        //
        while(g_ulFlags != (BUFFER_TOP_EMPTY | BUFFER_BOTTOM_EMPTY))
        {
        }

        //
        // Disable the Class D amp to save power.
        //
        SoundClassDDis();
        bRetcode = true;
    }

    //
    // Reenable the I2S interrupt now that we're finished mucking with
    // state and flags.
    // Same is done in sound.c so it may be safe to ommit
    //
    ROM_IntEnable(INT_I2S0);

    return(bRetcode);
}
