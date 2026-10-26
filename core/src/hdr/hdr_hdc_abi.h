// The part of FAAD2's C interface (neaacdec.h, FAAD2 2.7 to 2.11) that Eye of SaurCom calls, declared here by hand: no FAAD2 header and no FAAD2 or
// nrsc5 source is part of Eye of SaurCom. The library itself is the user's own build of FAAD2 with the HDC patch (docs/CODECS.md, "HD Radio sound").
//
// Why the frame info is oversized: the fields up to samplerate have kept their place from 2.7 to 2.11, the tail (channel layout, PS flags) grew
// between releases. Eye of SaurCom only reads the stable head and hands the library a buffer big enough for any tail, so a newer library cannot write
// past it. A library whose head does not look right is refused at load (hdr_hdc.cpp, plausible()).
#pragma once

namespace dect2 { namespace hdr {

typedef void* FaadHandle;               // NeAACDecHandle

struct FaadFrameInfo {                  // NeAACDecFrameInfo, head as in faad2 2.10 / 2.11
    unsigned long bytesconsumed;
    unsigned long samples;              // all channels together
    unsigned char channels;
    unsigned char error;                // 0 = ok, else an index of NeAACDecGetErrorMessage
    unsigned long samplerate;
    unsigned char tail[512];            // sbr, object type, header type, channel positions, ps ... (not read)
};

#if defined(_WIN32)
#define DECT2_FAAD_CALL __cdecl         // NEAACDECAPI is empty on the MinGW and MSVC builds of FAAD2: plain cdecl
#else
#define DECT2_FAAD_CALL
#endif

// nrsc5's patch adds NeAACDecInitHDC(&handle): it allocates a decoder set up for HDC (no ADTS header, fixed SBR / 2048-sample frames)
typedef void (DECT2_FAAD_CALL *FaadInitHdcFn)(FaadHandle* h);
typedef void* (DECT2_FAAD_CALL *FaadDecodeFn)(FaadHandle h, FaadFrameInfo* info, unsigned char* buffer, unsigned long size);
typedef void (DECT2_FAAD_CALL *FaadCloseFn)(FaadHandle h);
typedef const char* (DECT2_FAAD_CALL *FaadErrMsgFn)(unsigned char error);
typedef int (DECT2_FAAD_CALL *FaadVersionFn)(const char** id, const char** copyright);   // optional (2.8 and later)

}} // namespace dect2::hdr
