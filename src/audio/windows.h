
/*********************************************************************************************************************
** Hardware/platform related functions.
*/

int sndCheckActivity(struct PlatformData *);
extern "C" const char * sndCreateBuffer(Object *, void *, int, int, struct PlatformData *, int);
void sndFree(struct PlatformData *);
void sndFrequency(struct PlatformData *, int);
void sndSetPosition(struct PlatformData *, int);
void sndPan(struct PlatformData *, float);
__declspec(no_sanitize_address) int sndPlay(struct PlatformData *, bool, int);
void sndStop(struct PlatformData *);
extern "C" int sndStreamAudio(struct PlatformData *);
void sndVolume(struct PlatformData *, float);
void sndLength(struct PlatformData *, int);
extern "C" void end_of_stream(Object *, int);
extern "C" int sndBeep(int, int);
