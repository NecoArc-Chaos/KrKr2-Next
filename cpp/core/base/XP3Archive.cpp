//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// XP3 virtual file system support
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include "XP3Archive.h"
#include "MaitetsuCxDecryption.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "UtilStreams.h"
#include "SysInitIntf.h"

#include <zlib.h>
#include <algorithm>
#include <limits>
#include <mutex>

#include "TVPMmapAlloc.h"

bool TVPAllowExtractProtectedStorage = true;

static constexpr tjs_uint64 TVP_MAX_XP3_INDEX_SIZE = 256ULL * 1024ULL * 1024ULL;

//---------------------------------------------------------------------------
// archive filter related
//---------------------------------------------------------------------------
tTVPXP3ArchiveExtractionFilter TVPXP3ArchiveExtractionFilter = nullptr;

void TVPSetXP3ArchiveExtractionFilter(tTVPXP3ArchiveExtractionFilter filter) {
    TVPXP3ArchiveExtractionFilter = filter;
}
//---------------------------------------------------------------------------

static tTVPXP3ArchiveContentFilter TVPXP3ArchiveContentFilter = nullptr;

void TVPSetXP3ArchiveContentFilter(tTVPXP3ArchiveContentFilter filter) {
    TVPXP3ArchiveContentFilter = filter;
}

//---------------------------------------------------------------------------
// tTVPXP3ArchiveHandleCache
//---------------------------------------------------------------------------
#define TVP_MAX_ARCHIVE_HANDLE_CACHE 8
static tjs_uint TVPArchiveHandleCacheAge = 0;
struct tTVPArchiveHandleCacheItem {
    void *Pointer;
    tTJSBinaryStream *Stream;
    tjs_uint Age;
};
//---------------------------------------------------------------------------
static tTVPArchiveHandleCacheItem *TVPArchiveHandleCachePool = nullptr;
static bool TVPArchiveHandleCacheInit = false;
static bool TVPArchiveHandleCacheShutdown = false;
static tTJSCriticalSection TVPArchiveHandleCacheCS;

//---------------------------------------------------------------------------
tTJSBinaryStream *TVPGetCachedArchiveHandle(void *pointer, const ttstr &name) {
    // get cached archive file handle from the pool
    {
        tTJSCriticalSectionHolder cs_holder(TVPArchiveHandleCacheCS);
        if(!TVPArchiveHandleCacheShutdown) {
            if(!TVPArchiveHandleCacheInit) {
                // initialize the pool
                TVPArchiveHandleCachePool =
                    new tTVPArchiveHandleCacheItem[TVP_MAX_ARCHIVE_HANDLE_CACHE];
                for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
                    TVPArchiveHandleCachePool[i].Pointer = nullptr;
                    TVPArchiveHandleCachePool[i].Stream = nullptr;
                    TVPArchiveHandleCachePool[i].Age = 0;
                }
                TVPArchiveHandleCacheInit = true;
            }

            // linear search wiil be enough here because the
            // TVP_MAX_ARCHIVE_HANDLE_CACHE is relatively small
            for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
                tTVPArchiveHandleCacheItem *item = TVPArchiveHandleCachePool + i;
                if(item->Stream && item->Pointer == pointer) {
                    // found in the pool
                    tTJSBinaryStream *stream = item->Stream;
                    item->Stream = nullptr;
                    return stream;
                }
            }
        }
    }

    // not found in the pool
    // simply create a stream and return it
    return TVPCreateStream(name);
}
//---------------------------------------------------------------------------
/*static*/ void TVPReleaseCachedArchiveHandle(void *pointer,
                                              tTJSBinaryStream *stream) {
    // release archive file handle
    if(!stream)
        return;
    {
        tTJSCriticalSectionHolder cs_holder(TVPArchiveHandleCacheCS);
        if(!TVPArchiveHandleCacheShutdown && TVPArchiveHandleCacheInit) {
            // search empty cell in the pool
            tjs_uint oldest_age = 0;
            tjs_int oldest = 0;
            for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
                tTVPArchiveHandleCacheItem *item = TVPArchiveHandleCachePool + i;
                if(item->Stream == nullptr) {
                    // found the empty cell; fill it
                    item->Pointer = pointer;
                    item->Stream = stream;
                    item->Age = ++TVPArchiveHandleCacheAge;
                    return;
                }

                if(i == 0 || oldest_age > item->Age) {
                    oldest_age = item->Age;
                    oldest = i;
                }
            }

            // empty cell not found; free the oldest cell and fill it
            tTVPArchiveHandleCacheItem *oldest_item =
                TVPArchiveHandleCachePool + oldest;
            delete oldest_item->Stream;
            oldest_item->Pointer = pointer;
            oldest_item->Stream = stream;
            oldest_item->Age = ++TVPArchiveHandleCacheAge;
            return;
        }
    }

    delete stream;
}
//---------------------------------------------------------------------------
/*static*/ void TVPFreeArchiveHandlePoolByPointer(void *pointer) {
    // free all streams which have specified pointer
    tTJSCriticalSectionHolder cs_holder(TVPArchiveHandleCacheCS);
    if(TVPArchiveHandleCacheShutdown)
        return;
    if(!TVPArchiveHandleCacheInit)
        return;

    for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
        tTVPArchiveHandleCacheItem *item = TVPArchiveHandleCachePool + i;
        if(item->Stream && item->Pointer == pointer) {
            delete item->Stream, item->Stream = nullptr;
            item->Pointer = nullptr;
            item->Age = 0;
        }
    }
}

//---------------------------------------------------------------------------
static void TVPFreeArchiveHandlePool() {
    // free all streams
    tTJSCriticalSectionHolder cs_holder(TVPArchiveHandleCacheCS);
    if(TVPArchiveHandleCacheShutdown)
        return;
    if(!TVPArchiveHandleCacheInit)
        return;

    for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
        tTVPArchiveHandleCacheItem *item = TVPArchiveHandleCachePool + i;
        if(item->Stream) {
            delete item->Stream, item->Stream = nullptr;
            item->Pointer = nullptr;
            item->Age = 0;
        }
    }
}

//---------------------------------------------------------------------------
static void TVPShutdownArchiveHandleCache() {
    // free all stream and shutdown the pool
    tTJSCriticalSectionHolder cs_holder(TVPArchiveHandleCacheCS);

    TVPArchiveHandleCacheShutdown = true;
    if(!TVPArchiveHandleCacheInit)
        return;

    for(tjs_int i = 0; i < TVP_MAX_ARCHIVE_HANDLE_CACHE; i++) {
        if(TVPArchiveHandleCachePool[i].Stream)
            delete TVPArchiveHandleCachePool[i].Stream;
    }
    delete[] TVPArchiveHandleCachePool;
    TVPArchiveHandleCachePool = nullptr;
    TVPArchiveHandleCacheInit = false;
}

//---------------------------------------------------------------------------
static tTVPAtExit TVPShutdownArchiveCacheAtExit(TVP_ATEXIT_PRI_CLEANUP,
                                                TVPShutdownArchiveHandleCache);
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPXP3Archive
//---------------------------------------------------------------------------
/*
        TVP XPK3 virtual file system support. (in short : XP3)
        TVP supports no longer archive type of "XPK1/XPK2"
        ( XPK1/XPK2 is used by TVP ver under 0.9x ).

        here word "in-archive" is used for the storages which are
   contained in archive.
*/
//---------------------------------------------------------------------------
bool TVPGetXP3ArchiveOffset(tTJSBinaryStream *st, const ttstr name,
                            tjs_uint64 &offset, bool raise) {
    st->SetPosition(0);
    tjs_uint8 mark[11 + 1];
    static tjs_uint8 XP3Mark1[] = { 0x58 /*'X'*/,  0x50 /*'P'*/,
                                    0x33 /*'3'*/,  0x0d /*'\r'*/,
                                    0x0a /*'\n'*/, 0x20 /*' '*/,
                                    0x0a /*'\n'*/, 0x1a /*EOF*/,
                                    0xff /* sentinel */ };
    static tjs_uint8 XP3Mark2[] = { 0x8b, 0x67, 0x01, 0xff /* sentinel */ };

    // XP3 header mark contains:
    // 1. line feed and carriage return to detect corruption by
    // unnecessary
    //    line-feeds convertion
    // 2. 1A EOF mark which indicates file's text readable header
    // ending.
    // 3. 8B67 KANJI-CODE to detect curruption by unnecessary code
    // convertion
    // 4. 01 file structure version and character coding
    //    higher 4 bits are file structure version, currently 0.
    //    lower 4 bits are character coding, currently 1, is BMP 16bit
    //    Unicode.

    static tjs_uint8 XP3Mark[11 + 1];
    // +1: I was warned by CodeGuard that the code will do
    // access overrun... because a number of 11 is not aligned by
    // DWORD, and the processor may read the value of DWORD at last of
    // this array from offset 8. Then the last 1 byte would cause a
    // fail.
    static std::once_flag markInit;
    std::call_once(markInit, [] {
        // the XP3 header above is splitted into two part; to avoid
        // mis-finding of the header in the program's initialized data
        // area.
        memcpy(XP3Mark, XP3Mark1, 8);
        memcpy(XP3Mark + 8, XP3Mark2, 3);
    });

    memset(mark, 0, sizeof(mark));
    if(st->Read(mark, 11) != 11) {
        if(raise)
            TVPThrowExceptionMessage(TVPReadError, name);
        return false;
    }
    if(mark[0] == 0x4d /*'M'*/ && mark[1] == 0x5a /*'Z'*/) {
        // "MZ" is a mark of Win32/DOS executables,
        // TVP searches the first mark of XP3 archive
        // in the executeble file.
        bool found = false;

        offset = 16;
        st->SetPosition(16);

        // XP3 mark must be aligned by a paragraph ( 16 bytes )
        const tjs_uint one_read_size = 256 * 1024;
        tjs_uint read;
        tjs_uint carry = 0;
        tjs_uint64 scan_position = 16;
        tjs_uint8 *buffer =
            new tjs_uint8[one_read_size + 10]; // read 256kbytes at once

        while(0 != (read = st->Read(buffer + carry, one_read_size))) {
            const tjs_uint total = carry + read;
            const tjs_uint64 block_start = scan_position - carry;
            tjs_uint p = static_cast<tjs_uint>((16 - (block_start % 16)) % 16);
            while(p + 11 <= total) {
                if(!memcmp(XP3Mark, buffer + p, 11)) {
                    // found the mark
                    offset = block_start + p;
                    found = true;
                    break;
                }
                p += 16;
            }
            if(found)
                break;
            if(total > 10) {
                memmove(buffer, buffer + total - 10, 10);
                carry = 10;
            } else {
                carry = total;
            }
            scan_position += read;
        }
        delete[] buffer;
        if(!found) {
            if(raise)
                TVPThrowExceptionMessage(TVPCannotUnbindXP3EXE, name);
            else
                return false;
        }
    } else if(!memcmp(XP3Mark, mark, 11)) {
        // XP3 mark found
        offset = 0;
    } else {
        if(raise) {
            ttstr msg(TVPGetMessageByLocale("err_not_xp3_archive"));
            TVPThrowExceptionMessage(msg.c_str(), name);
            // TVPThrowExceptionMessage(TVPCannotFindXP3Mark, name);
        }
        return false;
    }

    return true;
}

//---------------------------------------------------------------------------
bool TVPIsXP3Archive(const ttstr &name) {
    tTVPStreamHolder holder(name);
    try {
        tjs_uint64 offset;
        return TVPGetXP3ArchiveOffset(holder.Get(), name, offset, false);
    } catch(...) {
        return false;
    }
}

//---------------------------------------------------------------------------
void tTVPXP3Archive::Init(tTJSBinaryStream *st, tjs_int64 off,
                          bool normalizeName) {
    tjs_uint64 offset = off;

    tjs_uint8 *indexdata = nullptr;

    static const tjs_uint8 cn_File[] = { 0x46 /*'F'*/, 0x69 /*'i'*/,
                                         0x6c /*'l'*/, 0x65 /*'e'*/ };
    static const tjs_uint8 cn_info[] = { 0x69 /*'i'*/, 0x6e /*'n'*/,
                                         0x66 /*'f'*/, 0x6f /*'o'*/ };
    static const tjs_uint8 cn_segm[] = { 0x73 /*'s'*/, 0x65 /*'e'*/,
                                         0x67 /*'g'*/, 0x6d /*'m'*/ };
    static const tjs_uint8 cn_adlr[] = { 0x61 /*'a'*/, 0x64 /*'d'*/,
                                         0x6c /*'l'*/, 0x72 /*'r'*/ };

    TVPAddLog(TVPFormatMessage(
        TVPInfoTryingToReadXp3VirtualFileSystemInformationFrom, ArchiveName));

    int segmentcount = 0;
    try {
        // retrieve archive offset
        if(off < 0)
            TVPGetXP3ArchiveOffset(st, ArchiveName, offset, true);

        const tjs_uint64 stream_size = st->GetSize();
        if(offset > stream_size || stream_size - offset < 11)
            TVPThrowExceptionMessage(TVPReadError);

        // read index position and seek
        st->SetPosition(11 + offset);

        // read all XP3 indices
        while(true) {
            if(indexdata)
                delete[] indexdata;

            tjs_uint64 index_ofs = st->ReadI64LE();
            if(index_ofs > stream_size - offset)
                TVPThrowExceptionMessage(TVPReadError);
            const tjs_uint64 index_position = index_ofs + offset;
            if(index_position >= stream_size)
                TVPThrowExceptionMessage(TVPReadError);
            st->SetPosition(index_position);

            // read index to memory
            tjs_uint8 index_flag;
            st->ReadBuffer(&index_flag, 1);
            tjs_uint index_size;

            if((index_flag & TVP_XP3_INDEX_ENCODE_METHOD_MASK) ==
               TVP_XP3_INDEX_ENCODE_ZLIB) {
                // compressed index
                tjs_uint64 compressed_size = st->ReadI64LE();
                tjs_uint64 r_index_size = st->ReadI64LE();

                if(compressed_size > stream_size - st->GetPosition() ||
                   compressed_size > TVP_MAX_XP3_INDEX_SIZE ||
                   r_index_size > std::numeric_limits<tjs_uint>::max() ||
                   r_index_size > TVP_MAX_XP3_INDEX_SIZE ||
                   r_index_size > std::numeric_limits<unsigned long>::max() ||
                   compressed_size > std::numeric_limits<tjs_uint>::max() ||
                   compressed_size > std::numeric_limits<unsigned long>::max())
                    TVPThrowExceptionMessage(TVPReadError);
                // too large to handle, or corrupted
                index_size = static_cast<tjs_uint>(r_index_size);
                indexdata = new tjs_uint8[index_size];
                tjs_uint8 *compressed = new tjs_uint8[
                    static_cast<tjs_uint>(compressed_size)];
                try {
                    st->ReadBuffer(compressed,
                                   static_cast<tjs_uint>(compressed_size));

                    unsigned long destlen = (unsigned long)index_size;

                    int result =
                        uncompress(/* uncompress from zlib */
                                   (unsigned char *)indexdata, &destlen,
                                   (unsigned char *)compressed,
                                   static_cast<unsigned long>(compressed_size));
                    if(result != Z_OK || destlen != (unsigned long)index_size)
                        TVPThrowExceptionMessage(TVPUncompressionFailed);
                } catch(...) {
                    delete[] compressed;
                    throw;
                }
                delete[] compressed;
            } else if((index_flag & TVP_XP3_INDEX_ENCODE_METHOD_MASK) ==
                      TVP_XP3_INDEX_ENCODE_RAW) {
                // uncompressed index
                tjs_uint64 r_index_size = st->ReadI64LE();
                if(r_index_size > std::numeric_limits<tjs_uint>::max() ||
                   r_index_size > TVP_MAX_XP3_INDEX_SIZE ||
                   r_index_size > stream_size - st->GetPosition())
                    TVPThrowExceptionMessage(TVPReadError);
                // too large to handle or corrupted
                index_size = static_cast<tjs_uint>(r_index_size);
                indexdata = new tjs_uint8[index_size];
                st->ReadBuffer(indexdata, index_size);
            } else {
                // unknown encode method
                TVPThrowExceptionMessage(TVPReadError);
            }

            // read index information from memory
            tjs_uint ch_file_start = 0;
            tjs_uint ch_file_size = index_size;
            // Count = 0;
            for(;;) {
                // find 'File' chunk
                if(!FindChunk(indexdata, cn_File, ch_file_start, ch_file_size))
                    break; // not found

                // find 'info' sub-chunk
                tjs_uint ch_info_start = ch_file_start;
                tjs_uint ch_info_size = ch_file_size;
                if(!FindChunk(indexdata, cn_info, ch_info_start, ch_info_size))
                    TVPThrowExceptionMessage(TVPReadError);

                // read info sub-chunk
                tArchiveItem item;
                if(ch_info_size < 22)
                    TVPThrowExceptionMessage(TVPReadError);
                tjs_uint32 flags =
                    ReadI32FromMem(indexdata + ch_info_start + 0);
                if(!TVPAllowExtractProtectedStorage &&
                   (flags & TVP_XP3_FILE_PROTECTED))
                    TVPThrowExceptionMessage(
                        TVPSpecifiedStorageHadBeenProtected);
                item.OrgSize = ReadI64FromMem(indexdata + ch_info_start + 4);
                item.ArcSize = ReadI64FromMem(indexdata + ch_info_start + 12);

                tjs_int len = ReadI16FromMem(indexdata + ch_info_start + 20);
                if(len < 0 || static_cast<tjs_uint64>(len) * 2 >
                                  ch_info_size - 22)
                    TVPThrowExceptionMessage(TVPReadError);
                ttstr name = TVPStringFromBMPUnicode(
                    (const tjs_uint16 *)(indexdata + ch_info_start + 22), len);
                item.Name = name;
                if(normalizeName)
                    NormalizeInArchiveStorageName(item.Name);

                // find 'segm' sub-chunk
                // Each of in-archive storages can be splitted into
                // some segments. Each segment can be compressed or
                // uncompressed independently. segments can share
                // partial area of archive storage. ( this is used for
                // OggVorbis' VQ code book sharing )
                tjs_uint ch_segm_start = ch_file_start;
                tjs_uint ch_segm_size = ch_file_size;
                if(!FindChunk(indexdata, cn_segm, ch_segm_start, ch_segm_size))
                    TVPThrowExceptionMessage(TVPReadError);

                // read segm sub-chunk
                if((ch_segm_size == 0 && item.OrgSize != 0) ||
                   (ch_segm_size != 0 && ch_segm_size % 28 != 0))
                    TVPThrowExceptionMessage(TVPReadError);
                tjs_int segment_count = ch_segm_size / 28;
                tjs_uint64 offset_in_archive = 0;
                for(tjs_int i = 0; i < segment_count; i++) {
                    tjs_uint pos_base = i * 28 + ch_segm_start;
                    tTVPXP3ArchiveSegment seg;
                    tjs_uint32 flags = ReadI32FromMem(indexdata + pos_base);

                    if((flags & TVP_XP3_SEGM_ENCODE_METHOD_MASK) ==
                       TVP_XP3_SEGM_ENCODE_RAW)
                        seg.IsCompressed = false;
                    else if((flags & TVP_XP3_SEGM_ENCODE_METHOD_MASK) ==
                            TVP_XP3_SEGM_ENCODE_ZLIB)
                        seg.IsCompressed = true;
                    else
                        TVPThrowExceptionMessage(
                            TVPReadError); // unknown encode method

                    const tjs_uint64 segment_start =
                        ReadI64FromMem(indexdata + pos_base + 4);
                    if(segment_start > std::numeric_limits<tjs_uint64>::max() -
                                           offset)
                        TVPThrowExceptionMessage(TVPReadError);
                    seg.Start = segment_start + offset;
                    // data offset in archive
                    seg.Offset = offset_in_archive; // offset in in-archive
                                                    // storage
                    seg.OrgSize = ReadI64FromMem(indexdata + pos_base +
                                                 12); // original size
                    seg.ArcSize = ReadI64FromMem(indexdata + pos_base +
                                                 20); // archived size
                    if(seg.OrgSize > std::numeric_limits<tjs_uint>::max() ||
                       seg.ArcSize > std::numeric_limits<tjs_uint>::max() ||
                       seg.OrgSize > std::numeric_limits<unsigned long>::max() ||
                       seg.ArcSize > std::numeric_limits<unsigned long>::max() ||
                       (!seg.IsCompressed && seg.OrgSize != seg.ArcSize) ||
                       seg.Start > stream_size ||
                       seg.ArcSize > stream_size - seg.Start ||
                       offset_in_archive >
                           std::numeric_limits<tjs_uint64>::max() - seg.OrgSize)
                        TVPThrowExceptionMessage(TVPReadError);
                    item.Segments.push_back(seg);
                    offset_in_archive += seg.OrgSize;
                    segmentcount++;
                }

                if(offset_in_archive != item.OrgSize)
                    TVPThrowExceptionMessage(TVPReadError);

                // find 'aldr' sub-chunk
                tjs_uint ch_adlr_start = ch_file_start;
                tjs_uint ch_adlr_size = ch_file_size;
                if(!FindChunk(indexdata, cn_adlr, ch_adlr_start, ch_adlr_size))
                    TVPThrowExceptionMessage(TVPReadError);

                // read 'aldr' sub-chunk
                if(ch_adlr_size < sizeof(tjs_int32))
                    TVPThrowExceptionMessage(TVPReadError);
                item.FileHash = ReadI32FromMem(indexdata + ch_adlr_start);

                // push information
                ItemVector.push_back(item);

                // to next file
                ch_file_start += ch_file_size;
                ch_file_size = index_size - ch_file_start;
                if(Count == std::numeric_limits<tjs_int>::max())
                    TVPThrowExceptionMessage(TVPReadError);
                Count++;
            }

            if(!(index_flag & TVP_XP3_INDEX_CONTINUE))
                break; // continue reading index when the bit sets
        }

        // sort item vector by its name (required for tTVPArchive
        // specification)
        if(normalizeName)
            std::stable_sort(ItemVector.begin(), ItemVector.end());
    } catch(...) {
        if(indexdata)
            delete[] indexdata;
        delete st;
        TVPAddLog((const tjs_char *)TVPInfoFailed);
        throw;
    }
    if(indexdata)
        delete[] indexdata;
    delete st;

    TVPAddLog(TVPFormatMessage(TVPInfoDoneWithContains, ttstr(Count),
                               ttstr(segmentcount)));
}

//---------------------------------------------------------------------------
tTVPXP3Archive::tTVPXP3Archive(const ttstr &name, tTJSBinaryStream *st,
                               tjs_int64 offset, bool normalizeFileName) :
    tTVPArchive(name) {
    if(!st)
        st = TVPCreateStream(name);
    Init(st, offset, normalizeFileName);
}

//---------------------------------------------------------------------------
tTVPXP3Archive::~tTVPXP3Archive() { TVPFreeArchiveHandlePoolByPointer(this); }

tTVPArchive *tTVPXP3Archive::Create(const ttstr &name, tTJSBinaryStream *st,
                                    bool normalizeFileName) {
    bool refStream = st;
    if(!st) {
        st = TVPCreateStream(name);
    }
    tjs_uint64 offset;
    if(!TVPGetXP3ArchiveOffset(st, name, offset, false)) {
        if(!refStream)
            delete st;
        return nullptr;
    }
    return new tTVPXP3Archive(name, st, offset, normalizeFileName);
}

//---------------------------------------------------------------------------
tTJSBinaryStream *tTVPXP3Archive::CreateStreamByIndex(tjs_uint idx) {
    if(idx >= ItemVector.size())
        TVPThrowExceptionMessage(TVPReadError);

    tArchiveItem &item = ItemVector[idx];

    if(item.OrgSize == 0)
        return new tTVPMemoryStream();
    if(item.Segments.empty())
        TVPThrowExceptionMessage(TVPReadError);

    tTJSBinaryStream *stream = TVPGetCachedArchiveHandle(this, ArchiveName);
    if(!stream)
        TVPThrowExceptionMessage(TVPReadError);

    tTVPXP3ArchiveStream *out = nullptr;
    try {
        out = new tTVPXP3ArchiveStream(this, idx, &(item.Segments), stream,
                                       item.OrgSize);
        if(TVPXP3ArchiveContentFilter) {
            tjs_int result = TVPXP3ArchiveContentFilter(
                item.Name, ArchiveName, item.OrgSize, &out->GetFilterContext());
#define XP3_CONTENT_FILTER_FETCH_FULLDATA 1
            if(result == XP3_CONTENT_FILTER_FETCH_FULLDATA) {
                if(item.OrgSize > std::numeric_limits<tjs_uint>::max())
                    TVPThrowExceptionMessage(TVPReadError);
                tTVPMemoryStream *memstr = new tTVPMemoryStream();
                try {
                    memstr->SetSize(static_cast<tjs_uint>(item.OrgSize));
                    out->ReadBuffer(memstr->GetInternalBuffer(),
                                    static_cast<tjs_uint>(item.OrgSize));
                } catch(...) {
                    delete memstr;
                    throw;
                }
                delete out;
                return memstr;
            }
        }
    } catch(...) {
        if(out)
            delete out;
        else
            TVPReleaseCachedArchiveHandle(this, stream);
        throw;
    }

    return out;
}

//---------------------------------------------------------------------------
bool tTVPXP3Archive::FindChunk(const tjs_uint8 *data, const tjs_uint8 *name,
                               tjs_uint &start, tjs_uint &size) {
    tjs_uint start_save = start;
    tjs_uint size_save = size;

    tjs_uint pos = 0;
    while(pos < size) {
        if(size - pos < 12)
            TVPThrowExceptionMessage(TVPReadError);
        bool found = !memcmp(data + start, name, 4);
        start += 4;
        tjs_uint64 r_size = ReadI64FromMem(data + start);
        start += 8;
        if(r_size > size - pos - 12 ||
           r_size > std::numeric_limits<tjs_uint>::max())
            TVPThrowExceptionMessage(TVPReadError);
        tjs_uint size_chunk = static_cast<tjs_uint>(r_size);
        if(found) {
            // found
            size = size_chunk;
            return true;
        }
        start += size_chunk;
        pos += size_chunk + 12;
    }

    start = start_save;
    size = size_save;
    return false;
}

//---------------------------------------------------------------------------
tjs_int16 tTVPXP3Archive::ReadI16FromMem(const tjs_uint8 *mem) {
    tjs_uint16 ret = (tjs_uint16)mem[0] | ((tjs_uint16)mem[1] << 8);
    return (tjs_int16)ret;
}

//---------------------------------------------------------------------------
tjs_int32 tTVPXP3Archive::ReadI32FromMem(const tjs_uint8 *mem) {
    tjs_uint32 ret = (tjs_uint32)mem[0] | ((tjs_uint32)mem[1] << 8) |
        ((tjs_uint32)mem[2] << 16) | ((tjs_uint32)mem[3] << 24);
    return (tjs_int32)ret;
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPXP3Archive::ReadI64FromMem(const tjs_uint8 *mem) {
    tjs_uint64 ret = (tjs_uint64)mem[0] | ((tjs_uint64)mem[1] << 8) |
        ((tjs_uint64)mem[2] << 16) | ((tjs_uint64)mem[3] << 24) |
        ((tjs_uint64)mem[4] << 32) | ((tjs_uint64)mem[5] << 40) |
        ((tjs_uint64)mem[6] << 48) | ((tjs_uint64)mem[7] << 56);
    return ret;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Compressed segment cache related
//---------------------------------------------------------------------------
#define TVP_SEGCACHE_ONE_LIMIT (1024 * 1024) // max size limit for each segment
#define TVP_SEGCACHE_TOTAL_LIMIT (1024 * 1024) // total segment cache size
tjs_uint TVPSegmentCacheLimit = TVP_SEGCACHE_TOTAL_LIMIT;

//---------------------------------------------------------------------------
struct tTVPSegmentCacheSearchData {
    ttstr Name; // archive name
    tjs_int StorageIndex; // storage index in archive
    tjs_int SegmentIndex; // segment index in storage

    bool operator==(const tTVPSegmentCacheSearchData &rhs) const {
        return Name == rhs.Name && StorageIndex == rhs.StorageIndex &&
            SegmentIndex == rhs.SegmentIndex;
    }
};

//---------------------------------------------------------------------------
class tTVPSegmentCacheSearchHashFunc {
public:
    static tjs_uint32 Make(const tTVPSegmentCacheSearchData &val) {
        tjs_uint32 v = tTJSHashFunc<ttstr>::Make(val.Name);

        v ^= val.StorageIndex;
        v ^= (val.SegmentIndex << 2);
        return v;
    }
};

//---------------------------------------------------------------------------
class tTVPSegmentData {
    tjs_int RefCount;
    tjs_uint Size;
    tjs_uint8 *Data;

public:
    tTVPSegmentData() {
        RefCount = 1;
        Size = 0;
        Data = nullptr;
    }

    ~tTVPSegmentData() {
        if(Data) {
#ifdef TVP_USE_MMAP_TEMP
            TVPMmapFree(Data);
#else
            delete[] Data;
#endif
        }
    }

    void SetData(unsigned long outsize, tTJSBinaryStream *instream,
                 unsigned long insize) {
        if(!instream || outsize == 0 || insize == 0)
            TVPThrowExceptionMessage(TVPReadError);
        tjs_uint8 *indata = nullptr;
        Data = nullptr;
        Size = 0;
        try {
#ifdef TVP_USE_MMAP_TEMP
            indata = (tjs_uint8 *)TVPMmapAlloc(insize);
#else
            indata = new tjs_uint8[insize];
#endif
            if(!indata)
                TVPThrowExceptionMessage(TVPInsufficientMemory);
            instream->ReadBuffer(indata, insize);

#ifdef TVP_USE_MMAP_TEMP
            Data = (tjs_uint8 *)TVPMmapAlloc(outsize);
#else
            Data = new tjs_uint8[outsize];
#endif
            if(!Data)
                TVPThrowExceptionMessage(TVPInsufficientMemory);
            unsigned long destlen = outsize;
            int result = uncompress((unsigned char *)Data, &destlen,
                                    (unsigned char *)indata, insize);
            if(result != Z_OK || destlen != outsize)
                TVPThrowExceptionMessage(TVPUncompressionFailed);
            Size = destlen;
        } catch(...) {
            if(Data) {
#ifdef TVP_USE_MMAP_TEMP
                TVPMmapFree(Data);
#else
                delete[] Data;
#endif
                Data = nullptr;
            }
#ifdef TVP_USE_MMAP_TEMP
            TVPMmapFree(indata);
#else
            delete[] indata;
#endif
            throw;
        }
#ifdef TVP_USE_MMAP_TEMP
        TVPMmapFree(indata);
#else
        delete[] indata;
#endif
    }

    const tjs_uint8 *GetData() const { return Data; }

    tjs_uint GetSize() const { return Size; }

    void AddRef() { RefCount++; }

    void Release() {
        if(RefCount == 1) {
            delete this;
        } else {
            RefCount--;
        }
    }
};

//---------------------------------------------------------------------------
typedef tTJSRefHolder<tTVPSegmentData> tTVPSegmentDataHolder;

typedef tTJSHashTable<tTVPSegmentCacheSearchData, tTVPSegmentDataHolder,
                      tTVPSegmentCacheSearchHashFunc>
    tTVPSegmentCache;
static tTVPSegmentCache TVPSegmentCache;
static tjs_uint TVPSegmentCacheTotalBytes = 0;

static tTJSCriticalSection TVPSegmentCacheCS;

//---------------------------------------------------------------------------
static void TVPCheckSegmentCacheLimitLocked() {
    while(TVPSegmentCacheTotalBytes > TVPSegmentCacheLimit) {
        // chop last segment
        tTVPSegmentCache::tIterator i;
        i = TVPSegmentCache.GetLast();
        if(!i.IsNull()) {
            tjs_uint size = i.GetValue().GetObjectNoAddRef()->GetSize();
            TVPSegmentCacheTotalBytes -= size;
            TVPSegmentCache.ChopLast(1);
        } else {
            break;
        }
    }
}

static void TVPCheckSegmentCacheLimit() {
    tTJSCriticalSectionHolder cs_holder(TVPSegmentCacheCS);
    TVPCheckSegmentCacheLimitLocked();
}

//---------------------------------------------------------------------------
void TVPClearXP3SegmentCache() {
    tTJSCriticalSectionHolder cs_holder(TVPSegmentCacheCS);

    TVPSegmentCache.Clear();
    TVPSegmentCacheTotalBytes = 0;
}

tjs_uint TVPGetXP3SegmentCacheTotalBytes() {
    tTJSCriticalSectionHolder cs_holder(TVPSegmentCacheCS);
    return TVPSegmentCacheTotalBytes;
}

//---------------------------------------------------------------------------
struct tTVPClearSegmentCacheCallback : public tTVPCompactEventCallbackIntf {
    void OnCompact(tjs_int level) override {
        if(level >= TVP_COMPACT_LEVEL_DEACTIVATE) {
            // clear the segment cache on application deactivate
            TVPClearXP3SegmentCache();
            // also free archive handle pool
            TVPFreeArchiveHandlePool();
        }
    }
} static TVPClearSegmentCacheCallback;

static bool TVPClearSegmentCacheCallbackInit = false;

//---------------------------------------------------------------------------
static tTVPSegmentData *
TVPSearchFromSegmentCache(const tTVPSegmentCacheSearchData &sdata,
                          tjs_uint32 hash) {
    tTJSCriticalSectionHolder cs_holder(TVPSegmentCacheCS);

    tTVPSegmentDataHolder *ptr =
        TVPSegmentCache.FindAndTouchWithHash(sdata, hash);
    if(ptr) {
        // found in cache
        return ptr->GetObject(); // add-refed
    }

    return nullptr; // not found in cache
}

//---------------------------------------------------------------------------
static void TVPPushToSegmentCache(const tTVPSegmentCacheSearchData &sdata,
                                  tjs_uint32 hash, tTVPSegmentData *data) {
    if(!TVPClearSegmentCacheCallbackInit) {
        TVPAddCompactEventHook(&TVPClearSegmentCacheCallback);
        TVPClearSegmentCacheCallbackInit = true;
    }

    tTJSCriticalSectionHolder cs_holder(TVPSegmentCacheCS);

    // A concurrent miss can produce the same segment twice. Keep the first
    // cached value and let the current stream use its own decoded buffer.
    if(TVPSegmentCache.FindAndTouchWithHash(sdata, hash))
        return;

    const tjs_uint size = data->GetSize();
    if(size > std::numeric_limits<tjs_uint>::max() -
                  TVPSegmentCacheTotalBytes) {
        TVPSegmentCache.Clear();
        TVPSegmentCacheTotalBytes = 0;
    }
    tTVPSegmentDataHolder holder(data);
    TVPSegmentCache.AddWithHash(sdata, hash, holder);
    TVPSegmentCacheTotalBytes += size;

    TVPCheckSegmentCacheLimitLocked();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPXP3ArchiveStream : stream class for in-archive storage
//---------------------------------------------------------------------------
tTVPXP3ArchiveStream::tTVPXP3ArchiveStream(
    tTVPXP3Archive *owner, tjs_int storageindex,
    std::vector<tTVPXP3ArchiveSegment> *segments, tTJSBinaryStream *stream,
    tjs_uint64 orgsize) {
    StorageIndex = storageindex;
    Segments = segments;
    SegmentData = nullptr;
    CurSegmentNum = 0;
    if(!owner || !segments || segments->empty() || !stream)
        TVPThrowExceptionMessage(TVPReadError);
    CurSegment = &(Segments->operator[](0));
    SegmentPos = 0;
    SegmentRemain = CurSegment->OrgSize;
    SegmentOpened = false;
    CurPos = 0;

    LastOpenedSegmentNum = -1;

    Owner = owner;
    Owner->AddRef(); // hook
    Stream = stream;
    OrgSize = orgsize;
}

//---------------------------------------------------------------------------
tTVPXP3ArchiveStream::~tTVPXP3ArchiveStream() {
    TVPReleaseCachedArchiveHandle(Owner, Stream);
    Owner->Release(); // unhook
    if(SegmentData)
        SegmentData->Release();
}

//---------------------------------------------------------------------------
void tTVPXP3ArchiveStream::EnsureSegment() {
    // ensure accessing to current segment
    if(SegmentOpened)
        return;

    if(LastOpenedSegmentNum == CurSegmentNum) {
        if(!CurSegment->IsCompressed)
            Stream->SetPosition(CurSegment->Start + SegmentPos);
        return;
    }

    // erase buffer
    if(SegmentData)
        SegmentData->Release(), SegmentData = nullptr;

    // is compressed segment ?
    if(CurSegment->IsCompressed) {
        // a compressed segment

        if(CurSegment->OrgSize >= TVP_SEGCACHE_ONE_LIMIT) {
            // too large to cache
            if(CurSegment->OrgSize > std::numeric_limits<tjs_uint>::max() ||
               CurSegment->ArcSize > std::numeric_limits<tjs_uint>::max())
                TVPThrowExceptionMessage(TVPReadError);
            Stream->SetPosition(CurSegment->Start);
            SegmentData = new tTVPSegmentData;
            try {
                SegmentData->SetData(static_cast<tjs_uint>(CurSegment->OrgSize),
                                     Stream,
                                     static_cast<tjs_uint>(CurSegment->ArcSize));
            } catch(...) {
                SegmentData->Release();
                SegmentData = nullptr;
                throw;
            }
        } else {
            // search thru segment cache
            tTVPSegmentCacheSearchData sdata;
            sdata.Name = Owner->GetName();
            sdata.StorageIndex = StorageIndex;
            sdata.SegmentIndex = CurSegmentNum;

            tjs_uint32 hash;
            hash = tTVPSegmentCacheSearchHashFunc::Make(sdata);

            SegmentData = TVPSearchFromSegmentCache(sdata, hash);
            if(!SegmentData) {
                // not found in cache
                if(CurSegment->OrgSize > std::numeric_limits<tjs_uint>::max() ||
                   CurSegment->ArcSize > std::numeric_limits<tjs_uint>::max())
                    TVPThrowExceptionMessage(TVPReadError);
                Stream->SetPosition(CurSegment->Start);
                SegmentData = new tTVPSegmentData;
                try {
                    SegmentData->SetData(
                        static_cast<tjs_uint>(CurSegment->OrgSize), Stream,
                        static_cast<tjs_uint>(CurSegment->ArcSize));
                } catch(...) {
                    SegmentData->Release();
                    SegmentData = nullptr;
                    throw;
                }

                // add to cache
                TVPPushToSegmentCache(sdata, hash, SegmentData);
            }
        }
    } else {
        // not a compressed segment

        Stream->SetPosition(CurSegment->Start + SegmentPos);
    }

    SegmentOpened = true;
    LastOpenedSegmentNum = CurSegmentNum;
}

//---------------------------------------------------------------------------
void tTVPXP3ArchiveStream::SeekToPosition(tjs_uint64 pos) {
    // open segment at 'pos' and seek
    // pos must between zero thru OrgSize
    if(CurPos == pos)
        return;

    // do binary search to determine current segment number
    tjs_int st = 0;
    tjs_int et = (tjs_int)Segments->size();
    tjs_int seg_num;

    while(true) {
        if(et - st <= 1) {
            seg_num = st;
            break;
        }
        tjs_int m = st + (et - st) / 2;
        if(Segments->operator[](m).Offset > pos)
            et = m;
        else
            st = m;
    }

    CurSegmentNum = seg_num;
    CurSegment = &(Segments->operator[](CurSegmentNum));
    SegmentOpened = false;

    SegmentPos = pos - CurSegment->Offset;
    SegmentRemain = CurSegment->OrgSize - SegmentPos;
    CurPos = pos;
}

//---------------------------------------------------------------------------
bool tTVPXP3ArchiveStream::OpenNextSegment() {
    // open next segment
    if(CurSegmentNum == (tjs_int)(Segments->size() - 1))
        return false; // no more segments
    CurSegmentNum++;
    CurSegment = &(Segments->operator[](CurSegmentNum));
    SegmentOpened = false;
    SegmentPos = 0;
    SegmentRemain = CurSegment->OrgSize;
    CurPos = CurSegment->Offset;
    EnsureSegment();
    return true;
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPXP3ArchiveStream::Seek(tjs_int64 offset, tjs_int whence) {
    tjs_uint64 newpos = CurPos;
    auto seek_from = [this](tjs_uint64 base, tjs_int64 delta,
                            tjs_uint64 &target) {
        if(delta < 0) {
            const tjs_uint64 distance =
                static_cast<tjs_uint64>(-(delta + 1)) + 1;
            if(distance > base)
                return false;
            target = base - distance;
            return true;
        }
        const tjs_uint64 distance = static_cast<tjs_uint64>(delta);
        if(distance > OrgSize - base)
            return false;
        target = base + distance;
        return true;
    };
    switch(whence) {
        case TJS_BS_SEEK_SET:
            if(offset >= 0 && seek_from(0, offset, newpos))
                SeekToPosition(newpos);
            return CurPos;

        case TJS_BS_SEEK_CUR:
            if(seek_from(CurPos, offset, newpos))
                SeekToPosition(newpos);
            return CurPos;

        case TJS_BS_SEEK_END:
            if(seek_from(OrgSize, offset, newpos))
                SeekToPosition(newpos);
            return CurPos;
    }
    return CurPos;
}

//---------------------------------------------------------------------------
tjs_uint tTVPXP3ArchiveStream::Read(void *buffer, tjs_uint read_size) {
    if(CurPos >= OrgSize || read_size == 0)
        return 0;
    const tjs_uint64 remaining = OrgSize - CurPos;
    if(static_cast<tjs_uint64>(read_size) > remaining)
        read_size = static_cast<tjs_uint>(remaining);
    EnsureSegment();

    tjs_uint write_size = 0;
    while(read_size) {
        while(SegmentRemain == 0) {
            // must go next segment
            if(!OpenNextSegment()) // open next segment
                return write_size; // could not read more
        }

        tjs_uint one_size =
            read_size > SegmentRemain ? (tjs_uint)SegmentRemain : read_size;

        if(CurSegment->IsCompressed) {
            // compressed segment; read from uncompressed data in
            // memory
            memcpy((tjs_uint8 *)buffer + write_size,
                   SegmentData->GetData() + (tjs_uint)SegmentPos, one_size);
        } else {
            // read directly from stream
            Stream->ReadBuffer((tjs_uint8 *)buffer + write_size, one_size);
        }

        // execute filter (for encryption method)
        if(TVPXP3ArchiveExtractionFilter) {
            tTVPXP3ExtractionFilterInfo info(
                CurPos, (tjs_uint8 *)buffer + write_size, one_size,
                Owner->GetFileHash(StorageIndex), Owner->GetName(StorageIndex));
            TVPXP3ArchiveExtractionFilter((tTVPXP3ExtractionFilterInfo *)&info,
                                          &FilterContext);
        } else {
            tTVPXP3ExtractionFilterInfo info(
                CurPos, (tjs_uint8 *)buffer + write_size, one_size,
                Owner->GetFileHash(StorageIndex), Owner->GetName(StorageIndex));
            MaitetsuCxExtractionFilter((tTVPXP3ExtractionFilterInfo *)&info);
        }

        // adjust members
        SegmentPos += one_size;
        CurPos += one_size;
        SegmentRemain -= one_size;
        read_size -= one_size;
        write_size += one_size;
    }

    return write_size;
}

//---------------------------------------------------------------------------
tjs_uint tTVPXP3ArchiveStream::Write(const void *buffer, tjs_uint write_size) {
    return 0;
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPXP3ArchiveStream::GetSize() { return OrgSize; }
//---------------------------------------------------------------------------
