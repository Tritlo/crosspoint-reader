#if defined(CROSSPOINT_EMULATOR)

#include <EmulatorNative.h>
#include <PNGdec.h>  // Keep the transitive EPUB decoder visible to PlatformIO's native LDF.

int main(int argc, char** argv) { return emulator::run(argc, argv); }

#endif
