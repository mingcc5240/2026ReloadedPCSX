#include <windows.h>
#include <stdint.h>

#define PSE_LT_CDR 1

struct CdrStat {
    uint32_t Type;
    uint32_t Status;
    unsigned char Time[3];
};

unsigned long CALLBACK PSEgetLibType(void) { return PSE_LT_CDR; }
char* CALLBACK PSEgetLibName(void) { return "Windows Null CDR Driver"; }
unsigned long CALLBACK PSEgetLibVersion(void) { return (2 << 16) | (1 << 8) | 0; }

long CALLBACK CDRinit(void) { return 0; }
long CALLBACK CDRshutdown(void) { return 0; }
long CALLBACK CDRopen(void) { return 0; }
long CALLBACK CDRclose(void) { return 0; }
long CALLBACK CDRgetTN(unsigned char *buffer) { return -1; }
long CALLBACK CDRgetTD(unsigned char track, unsigned char *buffer) { return -1; }
long CALLBACK CDRreadTrack(unsigned char *time) { return -1; }
unsigned char* CALLBACK CDRgetBuffer(void) { return NULL; }
unsigned char* CALLBACK CDRgetBufferSub(void) { return NULL; }
long CALLBACK CDRplay(unsigned char *time) { return 0; }
long CALLBACK CDRstop(void) { return 0; }
long CALLBACK CDRgetStatus(struct CdrStat *stat) {
    if (stat) {
        stat->Type = 0;
        stat->Status = 0;
    }
    return 0;
}
long CALLBACK CDRreadCDDA(unsigned char m, unsigned char s, unsigned char f, unsigned char *buffer) { return -1; }
char* CALLBACK CDRgetDriveLetter(void) { return NULL; }
long CALLBACK CDRconfigure(void) { return 0; }
long CALLBACK CDRtest(void) { return 0; }
void CALLBACK CDRabout(void) {
    MessageBoxA(NULL, "Windows Null CDR Plugin for PCSXR", "About", MB_OK);
}