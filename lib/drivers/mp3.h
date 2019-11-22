#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO

extern void Mp3PlayStart(unsigned char *pulAddress, const unsigned int len);
extern tBoolean Mp3PlayContinue();
