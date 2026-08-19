#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "UtilStreams.h"
#include "MsgIntf.h"
#include <algorithm>

extern "C" {
#include <7zip/C/7z.h>
#include <7zip/C/7zFile.h>
#include <7zip/C/7zCrc.h>
}

#include "StorageImpl.h"

#include <limits>
#include <memory>
#include <mutex>

static ISzAlloc allocImp = { [](ISzAllocPtr p, size_t size) -> void * {
                                return malloc(size);
                            },
                             [](ISzAllocPtr p, void *addr) { free(addr); } };

// SzArEx_Extract caches an entire solid folder in memory.  Keep malformed or
// hostile archives from turning that cache into an unbounded allocation.
static constexpr UInt64 kMax7zDecodeBlockSize = 512ULL * 1024ULL * 1024ULL;

class SevenZipStreamWrap {
public:
    CSzArEx db{};
    tTJSBinaryStream *_stream = nullptr;
    bool OwnsStream = false;
    std::recursive_mutex StreamMutex;
    CLookToRead2 lookStream{};
    struct CSeekInStream : public ISeekInStream {
        SevenZipStreamWrap *Host = nullptr;
    } archiveStream{};

public:
    SevenZipStreamWrap(tTJSBinaryStream *st) : _stream(st) {
        archiveStream.Host = this;
        archiveStream.Read = [](ISeekInStreamPtr p, void *buf,
                                size_t *size) -> SRes {
            return ((CSeekInStream *)p)->Host->StreamRead(buf, size);
        };
        archiveStream.Seek = [](ISeekInStreamPtr p, Int64 *pos,
                                ESzSeek origin) -> SRes {
            return ((CSeekInStream *)p)->Host->StreamSeek(pos, origin);
        };
        lookStream.buf = (Byte *)ISzAlloc_Alloc(&allocImp, 1 << 18);
        lookStream.bufSize = 1 << 18;
        if(!lookStream.buf)
            TVPThrowExceptionMessage(TVPInsufficientMemory);
        LookToRead2_CreateVTable(&lookStream, false);
        LookToRead2_INIT(&lookStream);
        lookStream.realStream = &archiveStream;
        SzArEx_Init(&db);
        static std::once_flag crcTableInit;
        std::call_once(crcTableInit, [] { CrcGenerateTable(); });
    }

    ~SevenZipStreamWrap() {
        SzArEx_Free(&db, &allocImp);
        if(lookStream.buf)
            ISzAlloc_Free(&allocImp, lookStream.buf);
        if(OwnsStream)
            delete _stream;
    }

    void TakeStreamOwnership() noexcept { OwnsStream = true; }

    SRes StreamRead(void *buf, size_t *size) {
        if(!_stream || !buf || !size)
            return SZ_ERROR_PARAM;
        std::lock_guard<std::recursive_mutex> lock(StreamMutex);
        try {
            const tjs_uint request = static_cast<tjs_uint>(std::min<size_t>(
                *size, std::numeric_limits<tjs_uint>::max()));
            *size = _stream->Read(buf, request);
            return SZ_OK;
        } catch(...) {
            *size = 0;
            return SZ_ERROR_READ;
        }
    }

    SRes StreamSeek(Int64 *pos, ESzSeek origin) {
        if(!pos)
            return SZ_ERROR_PARAM;
        tjs_int whence = TJS_BS_SEEK_SET;
        switch(origin) {
            case SZ_SEEK_CUR:
                whence = TJS_BS_SEEK_CUR;
                break;
            case SZ_SEEK_END:
                whence = TJS_BS_SEEK_END;
                break;
            case SZ_SEEK_SET:
                whence = TJS_BS_SEEK_SET;
                break;
            default:
                return SZ_ERROR_PARAM;
        }

        std::lock_guard<std::recursive_mutex> lock(StreamMutex);
        try {
            const tjs_uint64 result = _stream->Seek(*pos, whence);
            if(result > static_cast<tjs_uint64>(
                            std::numeric_limits<Int64>::max()))
                return SZ_ERROR_READ;
            *pos = static_cast<Int64>(result);
            return SZ_OK;
        } catch(...) {
            return SZ_ERROR_READ;
        }
    }
};

class SevenZipArchive : public tTVPArchive, public SevenZipStreamWrap {
    std::vector<std::pair<ttstr, tjs_uint>> filelist;

public:
    SevenZipArchive(const ttstr &name, tTJSBinaryStream *st) :
        tTVPArchive(name), SevenZipStreamWrap(st) {}

    ~SevenZipArchive() override {}

    tjs_uint GetCount() override {
        return static_cast<tjs_uint>(filelist.size());
    }

    ttstr GetName(tjs_uint idx) override {
        if(idx >= filelist.size())
            return {};
        return filelist[idx].first;
    }

    tTJSBinaryStream *CreateStreamByIndex(tjs_uint idx) override {
        if(idx >= filelist.size())
            return nullptr;
        std::lock_guard<std::recursive_mutex> lock(StreamMutex);
        tjs_uint fileIndex = filelist[idx].second;
        UInt64 fileSize = SzArEx_GetFileSize(&db, fileIndex);
        if(fileSize == 0)
            return new tTVPMemoryStream();
        if(fileSize > std::numeric_limits<tjs_uint>::max())
            return nullptr;

        if(fileIndex >= static_cast<UInt32>(db.NumFiles) ||
           !db.FileToFolder)
            return nullptr;
        UInt32 folderIndex = db.FileToFolder[fileIndex];
        if(folderIndex == (UInt32)-1 || folderIndex >= db.db.NumFolders ||
           db.db.FoCodersOffsets == nullptr || db.db.CodersData == nullptr ||
           db.db.FoStartPackStreamIndex == nullptr ||
           db.db.PackPositions == nullptr)
            return nullptr;

        const CSzAr *p = &db.db;
        CSzFolder folder;
        CSzData sd;
        const UInt64 coderStart = p->FoCodersOffsets[folderIndex];
        const UInt64 coderEnd = p->FoCodersOffsets[folderIndex + 1];
        if(coderEnd < coderStart ||
           coderStart > static_cast<UInt64>(std::numeric_limits<size_t>::max()) ||
           coderEnd - coderStart >
               static_cast<UInt64>(std::numeric_limits<size_t>::max()))
            return nullptr;
        const Byte *data = p->CodersData + coderStart;
        sd.Data = data;
        sd.Size = static_cast<size_t>(coderEnd - coderStart);

        if(SzGetNextFolderItem(&folder, &sd) != SZ_OK)
            return nullptr;
        if(folder.NumCoders == 1 && folder.NumPackStreams == 1) {
            UInt64 startPos = db.dataPos;
            const UInt64 packStreamIndex =
                p->FoStartPackStreamIndex[folderIndex];
            if(packStreamIndex >= p->NumPackStreams ||
               packStreamIndex + 1 < packStreamIndex ||
               packStreamIndex + 1 > p->NumPackStreams)
                return nullptr;
            const UInt64 *packPositions = p->PackPositions + packStreamIndex;
            UInt64 offset = packPositions[0];
            if(packPositions[1] < offset)
                return nullptr;
            UInt64 inSize = packPositions[1] - offset;
#define k_Copy 0
            if(folder.Coders[0].MethodID == k_Copy && inSize == fileSize) {
                const UInt64 streamSize = _stream->GetSize();
                if(startPos > streamSize || offset > streamSize - startPos ||
                   inSize > streamSize - startPos - offset)
                    return nullptr;
                if(startPos > std::numeric_limits<UInt64>::max() - offset)
                    return nullptr;
                return new TArchiveStream(this, startPos + offset, inSize);
            }
        }

        const UInt64 folderSize = SzAr_GetFolderUnpackSize(&db.db, folderIndex);
        if(folderSize > kMax7zDecodeBlockSize ||
           folderSize > std::numeric_limits<size_t>::max())
            return nullptr;

        UInt32 blockIndex = (UInt32)-1;
        Byte *outBuffer = nullptr;
        size_t outBufferSize = 0;
        size_t offset = 0;
        size_t outSizeProcessed = 0;
        SRes res = SzArEx_Extract(&db, &lookStream.vt, fileIndex, &blockIndex,
                                  &outBuffer, &outBufferSize, &offset,
                                  &outSizeProcessed, &allocImp, &allocImp);
        if(res != SZ_OK || !outBuffer || offset > outSizeProcessed ||
           fileSize > outSizeProcessed - offset) {
            if(outBuffer)
                ISzAlloc_Free(&allocImp, outBuffer);
            return nullptr;
        }

        const tjs_uint streamSize = static_cast<tjs_uint>(fileSize);
        tTVPMemoryStream *mem = nullptr;
        try {
            mem = new tTVPMemoryStream();
            mem->SetSize(streamSize);
            memcpy(mem->GetInternalBuffer(), outBuffer + offset, streamSize);
        } catch(...) {
            delete mem;
            ISzAlloc_Free(&allocImp, outBuffer);
            throw;
        }
        ISzAlloc_Free(&allocImp, outBuffer);
        return mem;
    }

    bool Open(bool normalizeFileName) {
        std::lock_guard<std::recursive_mutex> lock(StreamMutex);
        filelist.clear();
        SRes res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocImp);
        if(res != SZ_OK) {
            SzArEx_Free(&db, &allocImp);
            SzArEx_Init(&db);
            return false;
        }
        if(static_cast<UInt64>(db.NumFiles) >
               static_cast<UInt64>(std::numeric_limits<tjs_uint>::max())) {
            SzArEx_Free(&db, &allocImp);
            SzArEx_Init(&db);
            return false;
        }
        for(UInt32 i = 0; i < db.NumFiles; i++) {
            size_t offset = 0;
            size_t outSizeProcessed = 0;
            bool isDir = SzArEx_IsDir(&db, i);
            if(isDir)
                continue;
            size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            ttstr filename;
            SzArEx_GetFileNameUtf16(&db, i,
                                    (UInt16 *)filename.AllocBuffer(len));
            filename.FixLen();
            if(normalizeFileName)
                NormalizeInArchiveStorageName(filename);
            filelist.emplace_back(filename, static_cast<tjs_uint>(i));
        }
        if(normalizeFileName) {
            std::sort(filelist.begin(), filelist.end(),
                      [](const std::pair<ttstr, tjs_uint> &a,
                         const std::pair<ttstr, tjs_uint> &b) {
                          return a.first < b.first;
                      });
        }
        return true;
    }
};

tTVPArchive *TVPOpen7ZArchive(const ttstr &name, tTJSBinaryStream *st,
                              bool normalizeFileName) {
    tjs_uint64 pos = st->GetPosition();
    bool checkZIP = st->ReadI16LE() == 0x7A37; // '7z'
    st->SetPosition(pos);
    if(!checkZIP)
        return nullptr;
    std::unique_ptr<SevenZipArchive> arc(
        new SevenZipArchive(name, st));
    if(!arc->Open(normalizeFileName)) {
        return nullptr;
    }
    arc->TakeStreamOwnership();
    return arc.release();
}

#if 0
void TVPUnpack7ZArchive(tTJSBinaryStream *st, ttstr outpath) {
    tjs_uint64 origpos = st->GetPosition();
    SevenZipStreamWrap szsw(st);
    CSzArEx &db = szsw.db;
    SRes res = SzArEx_Open(&db, &szsw.lookStream.s, &allocImp, &allocImp);
    if (res != SZ_OK) return;
    outpath += TJS_W("/");
    for (int i = 0; i < db.db.NumFolders; ++i) {
        ;
    }
    for (int i = 0; i < db.NumFiles; i++) {
        size_t offset = 0;
        size_t outSizeProcessed = 0;
        size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        ttstr filename;
        SzArEx_GetFileNameUtf16(&db, i, (UInt16*)filename.AllocBuffer(len));
        filename.FixLen();
        bool isDir = SzArEx_IsDir(&db, i);
        ttstr fullpath = outpath + filename;
        if (isDir) {
            if (!TVPCheckExistentLocalFolder(fullpath))
                TVPCreateFolders(fullpath);
        } else {
            tjs_uint fileIndex = i;
            UInt64 fileSize = SzArEx_GetFileSize(&db, fileIndex);
            if (fileSize <= 0) {
                FILE *fp = fopen(fullpath.AsStdString().c_str(), "wb");
                fclose(fp);
            }

            UInt32 folderIndex = db.FileToFolder[fileIndex];
            if (folderIndex == (UInt32)-1) continue;

            const CSzAr *p = &db.db;
            CSzFolder folder;
            CSzData sd;
            const Byte *data = p->CodersData + p->FoCodersOffsets[folderIndex];
            sd.Data = data;
            sd.Size = p->FoCodersOffsets[folderIndex + 1] - p->FoCodersOffsets[folderIndex];

            if (SzGetNextFolderItem(&folder, &sd) != SZ_OK) continue;
            if (folder.NumCoders == 1) {
                UInt64 startPos = db.dataPos;
                const UInt64 *packPositions = p->PackPositions + p->FoStartPackStreamIndex[folderIndex];
                UInt64 offset = packPositions[0];
                UInt64 inSize = packPositions[1] - offset;
                if (folder.Coders[0].MethodID == k_Copy && inSize == fileSize) {
                    CopyStreamToFile(st, origpos + startPos + offset, inSize, fullpath);
                    continue;
                }
            }

            UInt32 blockIndex;
            Byte *outBuffer = nullptr;
            size_t outBufferSize;
            size_t offset, outSizeProcessed;
            SRes res = SzArEx_Extract(&db, &szsw.lookStream.s, fileIndex, &blockIndex, &outBuffer, &outBufferSize,
                &offset, &outSizeProcessed, &allocImp, &allocImp);
            tTVPMemoryStream *mem;
            if (offset == 0 && fileSize <= outBufferSize) {
                mem = new tTVPMemoryStream(outBuffer, outBufferSize);
            } else {
                Byte *buf = new Byte[fileSize];
                memcpy(buf, outBuffer, fileSize);
                mem = new tTVPMemoryStream(buf, fileSize);
                delete outBuffer;
            }
            return mem;
        }
    }
}
#endif
