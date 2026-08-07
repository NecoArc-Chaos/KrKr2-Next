//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Stream related utilities / utility streams
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "UtilStreams.h"
#include "MsgIntf.h"
#include "DebugIntf.h"

#include "TVPMmapAlloc.h"
#ifdef TVP_USE_MMAP_TEMP
static constexpr size_t kStreamMmapThreshold = 256 * 1024;
#endif
#include "EventIntf.h"
#include "StorageIntf.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include "Platform.h"

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

//---------------------------------------------------------------------------
// tTVPLocalTempStorageHolder
//---------------------------------------------------------------------------
#define TVP_LOCAL_TEMP_COPY_BLOCK_SIZE 65536 * 2

tTVPLocalTempStorageHolder::tTVPLocalTempStorageHolder(const ttstr &name) {
    // name must be normalized !!!

    FileMustBeDeleted = false;
    FolderMustBeDeleted = false;
    LocalName = TVPGetLocallyAccessibleName(name);
    if(LocalName.IsEmpty()) {
        // file must be copied to local filesystem

        // note that the basename is much more important than the
        // directory which the file is to be in, so we create a
        // temporary folder and store the file in it.

        LocalFolder = TVPGetTemporaryName();
        LocalName = LocalFolder + TJS_W("/") + TVPExtractStorageName(name);
        TVPCreateFolders(LocalFolder); // create temporary folder
        FolderMustBeDeleted = true;
        FileMustBeDeleted = true;

        try {
            // copy to local file
            tTVPStreamHolder src(name);
            tTVPStreamHolder dest(LocalName,
                                  TJS_BS_WRITE | TJS_BS_DELETE_ON_CLOSE);
            tjs_uint8 *buffer = new tjs_uint8[TVP_LOCAL_TEMP_COPY_BLOCK_SIZE];
            try {
                tjs_uint read;
                while(true) {
                    read = src->Read(buffer, TVP_LOCAL_TEMP_COPY_BLOCK_SIZE);
                    if(read == 0)
                        break;
                    dest->WriteBuffer(buffer, read);
                }
            } catch(...) {
                delete[] buffer;
                throw;
            }
            delete[] buffer;
        } catch(...) {
            if(FileMustBeDeleted)
                TVPRemoveFile(LocalName);
            if(FolderMustBeDeleted)
                TVPRemoveFolder(LocalFolder);
            throw;
        }
    }
}

//---------------------------------------------------------------------------
tTVPLocalTempStorageHolder::~tTVPLocalTempStorageHolder() {
    if(FileMustBeDeleted)
        TVPRemoveFile(LocalName);
    if(FolderMustBeDeleted)
        TVPRemoveFolder(LocalFolder);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPMemoryStream
//---------------------------------------------------------------------------
tTVPMemoryStream::tTVPMemoryStream() { Init(); }

//---------------------------------------------------------------------------
tTVPMemoryStream::tTVPMemoryStream(const void *block, tjs_uint size) {
    Init();
    Block = (void *)block;
    if(!Block) {
        Block = Alloc(size);
        if(!Block)
            TVPThrowExceptionMessage(TVPInsufficientMemory);
    } else {
        Reference = true; // memory block was given
    }
    Size = size;
    AllocSize = size;
    CurrentPos = 0;
}

//---------------------------------------------------------------------------
tTVPMemoryStream::~tTVPMemoryStream() {
    if(Block && !Reference)
        Free(Block);
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPMemoryStream::Seek(tjs_int64 offset, tjs_int whence) {
    auto seek_from = [this](tjs_uint64 base, tjs_int64 delta) {
        tjs_uint64 target = 0;
        if(delta < 0) {
            const tjs_uint64 distance =
                static_cast<tjs_uint64>(-(delta + 1)) + 1;
            if(distance > base)
                return;
            target = base - distance;
        } else {
            const tjs_uint64 distance = static_cast<tjs_uint64>(delta);
            if(distance > static_cast<tjs_uint64>(Size) - base)
                return;
            target = base + distance;
        }
        CurrentPos = static_cast<tjs_uint>(target);
    };

    switch(whence) {
        case TJS_BS_SEEK_SET:
            if(offset >= 0 && static_cast<tjs_uint64>(offset) <= Size)
                CurrentPos = static_cast<tjs_uint>(offset);
            return CurrentPos;

        case TJS_BS_SEEK_CUR:
            seek_from(CurrentPos, offset);
            return CurrentPos;

        case TJS_BS_SEEK_END:
            seek_from(Size, offset);
            return CurrentPos;
    }
    return CurrentPos;
}

//---------------------------------------------------------------------------
tjs_uint tTVPMemoryStream::Read(void *buffer, tjs_uint read_size) {
    if(CurrentPos >= Size)
        return 0;
    if(read_size > Size - CurrentPos)
        read_size = Size - CurrentPos;

    memcpy(buffer, (tjs_uint8 *)Block + CurrentPos, read_size);

    CurrentPos += read_size;

    return read_size;
}

//---------------------------------------------------------------------------
tjs_uint tTVPMemoryStream::Write(const void *buffer, tjs_uint write_size) {
    // writing may increase the internal buffer size.
    if(Reference)
        TVPThrowExceptionMessage(TVPWriteError);

    if(write_size > std::numeric_limits<tjs_uint>::max() - CurrentPos)
        TVPThrowExceptionMessage(TVPWriteError);

    const tjs_uint newpos = CurrentPos + write_size;
    if(newpos > AllocSize) {
        // exceeds AllocSize
        tjs_uint onesize;
        if(AllocSize < 64 * 1024)
            onesize = 4 * 1024;
        else if(AllocSize < 512 * 1024)
            onesize = 16 * 1024;
        else if(AllocSize < 4096 * 1024)
            onesize = 256 * 1024;
        else
            onesize = 2024 * 1024;
        tjs_uint new_alloc_size = AllocSize;
        if(onesize > std::numeric_limits<tjs_uint>::max() - new_alloc_size)
            new_alloc_size = std::numeric_limits<tjs_uint>::max();
        else
            new_alloc_size += onesize;
        if(new_alloc_size < newpos)
            new_alloc_size = newpos;
        if(new_alloc_size < newpos)
            TVPThrowExceptionMessage(TVPInsufficientMemory);

        void *new_block = Realloc(Block, new_alloc_size);
        if(new_alloc_size && !new_block)
            TVPThrowExceptionMessage(TVPInsufficientMemory);
        Block = new_block;
        AllocSize = new_alloc_size;
    }

    if(write_size > 0)
        memcpy((tjs_uint8 *)Block + CurrentPos, buffer, write_size);

    CurrentPos = newpos;

    if(CurrentPos > Size)
        Size = CurrentPos;

    return write_size;
}

//---------------------------------------------------------------------------
void tTVPMemoryStream::SetEndOfStorage() {
    if(Reference)
        TVPThrowExceptionMessage(TVPWriteError);

    const tjs_uint new_size = CurrentPos;
    void *new_block = Realloc(Block, new_size);
    if(new_size && !new_block)
        TVPThrowExceptionMessage(TVPInsufficientMemory);
    Block = new_block;
    Size = new_size;
    AllocSize = new_size;
}

//---------------------------------------------------------------------------
void tTVPMemoryStream::Clear() {
    if(Block && !Reference)
        Free(Block);
    Init();
}

//---------------------------------------------------------------------------
void tTVPMemoryStream::SetSize(tjs_uint size) {
    if(Reference)
        TVPThrowExceptionMessage(TVPWriteError);

    void *new_block = Realloc(Block, size);
    if(size && !new_block)
        TVPThrowExceptionMessage(TVPInsufficientMemory);
    Block = new_block;
    AllocSize = size;
    Size = size;
    if(CurrentPos > Size)
        CurrentPos = Size;
}

//---------------------------------------------------------------------------
void tTVPMemoryStream::Init() {
    Block = nullptr;
    Reference = false;
    Size = 0;
    AllocSize = 0;
    CurrentPos = 0;
    UseMmap = false;
}

//---------------------------------------------------------------------------
void *tTVPMemoryStream::Alloc(size_t size) {
#if defined(TVP_USE_MMAP_TEMP) && \
    (defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__))
    if(size >= kStreamMmapThreshold) {
        void *p = TVPMmapAlloc(size);
        if(p) { UseMmap = true; return p; }
    }
#endif
    UseMmap = false;
    return TJS_malloc(size);
}

//---------------------------------------------------------------------------
void *tTVPMemoryStream::Realloc(void *orgblock, size_t size) {
#if defined(TVP_USE_MMAP_TEMP) && \
    (defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__))
    if(UseMmap) {
        if(size == 0) {
            if(orgblock)
                TVPMmapFree(orgblock);
            UseMmap = false;
            return nullptr;
        }
        void *newblock = TVPMmapAlloc(size);
        if(!newblock) return nullptr;
        if(orgblock) {
            size_t copySize = (AllocSize < size) ? AllocSize : size;
            memcpy(newblock, orgblock, copySize);
            TVPMmapFree(orgblock);
        }
        return newblock;
    }
    if(size >= kStreamMmapThreshold && !orgblock) {
        void *p = TVPMmapAlloc(size);
        if(p) { UseMmap = true; return p; }
    }
#endif
    return TJS_realloc(orgblock, size);
}

//---------------------------------------------------------------------------
void tTVPMemoryStream::Free(void *block) {
#if defined(TVP_USE_MMAP_TEMP) && \
    (defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__))
    if(UseMmap) {
        TVPMmapFree(block);
        UseMmap = false;
        return;
    }
#endif
    TJS_free(block);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPPartialStream
//---------------------------------------------------------------------------
tTVPPartialStream::tTVPPartialStream(tTJSBinaryStream *stream, tjs_uint64 start,
                                     tjs_uint64 size) {
    // the stream given as a argument here will be owned by this
    // instance, and freed at the destruction.

    Stream = stream;
    Start = start;
    Size = size;
    CurrentPos = 0;

    try {
        Stream->SetPosition(Start);
    } catch(...) {
        delete Stream;
        Stream = nullptr;
        throw;
    }
}

//---------------------------------------------------------------------------
tTVPPartialStream::~tTVPPartialStream() {
    if(Stream)
        delete Stream;
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPPartialStream::Seek(tjs_int64 offset, tjs_int whence) {
    auto seek_from = [this](tjs_uint64 base, tjs_int64 delta) {
        tjs_uint64 target = 0;
        if(delta < 0) {
            const tjs_uint64 distance =
                static_cast<tjs_uint64>(-(delta + 1)) + 1;
            if(distance > base)
                return;
            target = base - distance;
        } else {
            const tjs_uint64 distance = static_cast<tjs_uint64>(delta);
            if(distance > Size - base)
                return;
            target = base + distance;
        }
        if(target > static_cast<tjs_uint64>(
                        std::numeric_limits<tjs_int64>::max()) ||
           Start > static_cast<tjs_uint64>(
                       std::numeric_limits<tjs_int64>::max()) - target)
            return;
        const tjs_uint64 absolute = Start + target;
        const tjs_uint64 actual =
            Stream->Seek(static_cast<tjs_int64>(absolute), TJS_BS_SEEK_SET);
        if(actual >= Start && actual - Start <= Size)
            CurrentPos = actual - Start;
    };

    switch(whence) {
        case TJS_BS_SEEK_SET:
            if(offset >= 0 && static_cast<tjs_uint64>(offset) <= Size)
                seek_from(0, offset);
            return CurrentPos;

        case TJS_BS_SEEK_CUR:
            seek_from(CurrentPos, offset);
            return CurrentPos;

        case TJS_BS_SEEK_END:
            seek_from(Size, offset);
            return CurrentPos;
    }
    return CurrentPos;
}

//---------------------------------------------------------------------------
tjs_uint tTVPPartialStream::Read(void *buffer, tjs_uint read_size) {
    if(CurrentPos >= Size)
        return 0;
    const tjs_uint64 remaining = Size - CurrentPos;
    if(static_cast<tjs_uint64>(read_size) > remaining)
        read_size = static_cast<tjs_uint>(std::min<tjs_uint64>(
            remaining, std::numeric_limits<tjs_uint>::max()));

    tjs_uint read = Stream->Read(buffer, read_size);

    CurrentPos += read;

    return read;
}

//---------------------------------------------------------------------------
tjs_uint tTVPPartialStream::Write(const void *buffer, tjs_uint write_size) {
    return 0;
}

//---------------------------------------------------------------------------
tjs_uint64 tTVPPartialStream::GetSize() { return Size; }
//---------------------------------------------------------------------------

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}
#if 0
class LibArchive_Archive : public tTVPArchive {
    struct archive *_arc;
    tTJSBinaryStream *_stream;
    static const int BUFFER_SIZE = 16 * 1024;
    tjs_uint8 *_buffer = new tjs_uint8[BUFFER_SIZE];
    std::vector<std::pair<ttstr, struct archive_entry *> > _filelist;

    void Clear() {
        if (_arc) {
            archive_read_free(_arc);
            _arc = nullptr;
        }
        for (std::pair<ttstr, struct archive_entry *> &it : _filelist) {
            archive_entry_free(it.second);
        }
        _filelist.clear();
    }

public:
    LibArchive_Archive(const ttstr & name, tTJSBinaryStream *st) : tTVPArchive(name), _stream(st) {
        _arc = archive_read_new();
    }
    ~LibArchive_Archive() {
        Clear();
        if (_stream)
            delete _stream;
        if (_buffer)
            delete[] _buffer;
    }

    virtual tjs_uint GetCount() { return _filelist.size(); }
    virtual ttstr GetName(tjs_uint idx) { return _filelist[idx].first; }
    virtual tTJSBinaryStream * CreateStreamByIndex(tjs_uint idx) {
        struct archive_entry * entry = _filelist[idx].second;
        tjs_uint64 fileSize = archive_entry_size(entry);
        if (fileSize <= 0) return new tTVPMemoryStream();
        return nullptr;
    }

    bool Open(bool normalizeFileName) {
        archive_read_support_filter_all(_arc);
        archive_read_support_format_all(_arc);
        archive_read_set_seek_callback(_arc, seek_callback);
        archive_read_open2(_arc, this, nullptr, read_callback, nullptr, nullptr);

        struct archive_entry *entry = archive_entry_new2(_arc);
        while (archive_read_next_header2(_arc, entry) == ARCHIVE_OK) {
            ttstr filename(archive_entry_pathname_utf8(entry));
            if (normalizeFileName)
                NormalizeInArchiveStorageName(filename);
            _filelist.emplace_back(filename, entry);
            entry = archive_entry_new2(_arc);
        }
        archive_entry_free(entry);
        if (normalizeFileName) {
            std::sort(_filelist.begin(), _filelist.end(), [](const std::pair<ttstr, struct archive_entry*>& a, const std::pair<ttstr, struct archive_entry*>& b) {
                return a.first < b.first;
            });
        }
    }

    void Detach() {
        Clear();
        _stream = nullptr;
    }

    static la_ssize_t read_callback(struct archive *, void *_client_data, const void **_buffer) {
        LibArchive_Archive *_this = (LibArchive_Archive *)_client_data;
        *_buffer = _this->_buffer;
        return _this->_stream->Read(_this->_buffer, BUFFER_SIZE);
    }

    static la_int64_t seek_callback(struct archive *, void *_client_data, la_int64_t offset, int whence) {
        LibArchive_Archive *_this = (LibArchive_Archive *)_client_data;
        return _this->_stream->Seek(offset, whence);
    }

    static la_int64_t skip_callback(struct archive *, void *_client_data, la_int64_t request) {
        LibArchive_Archive *_this = (LibArchive_Archive *)_client_data;
        return _this->_stream->Seek(request, TJS_BS_SEEK_CUR);
    }
};

tTVPArchive *TVPOpenLibArchive(const ttstr & name, tTJSBinaryStream *st, bool normalizeFileName) {
    LibArchive_Archive *arc = new LibArchive_Archive(name, st);
    if (arc->Open(normalizeFileName)) {
        return arc;
    }
    arc->Detach();
    delete arc;
    return nullptr;
}
#endif

static FILE *_fileopen(ttstr path) {
    std::string strpath = path.AsStdString();
    FILE *fp = fopen(strpath.c_str(), "wb");
    if(!fp) { // make dirs
        TVPCreateFolders(TVPExtractStoragePath(path));
        fp = fopen(strpath.c_str(), "wb");
    }
    return fp;
}

static bool ResolveArchivePath(const std::string &rootPath,
                               const std::string &entryName,
                               std::string &resolvedPath) {
    namespace fs = std::filesystem;
    if(entryName.empty() || entryName.find('\0') != std::string::npos)
        return false;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::absolute(fs::u8path(rootPath), ec),
                                         ec);
    if(ec)
        return false;

    const fs::path entry = fs::u8path(entryName);
    if(entry.is_absolute() || entry.has_root_name() ||
       entry.has_root_directory())
        return false;

    const fs::path lexicalTarget = (root / entry).lexically_normal();
    const fs::path lexicalRelative = lexicalTarget.lexically_relative(root);
    if(lexicalRelative.empty())
        return false;
    for(const fs::path &part : lexicalRelative) {
        if(part == fs::path(".."))
            return false;
    }

    // Resolve existing parent components as well, so a pre-existing symlink
    // under the extraction root cannot redirect the write outside it.
    const fs::path target = fs::weakly_canonical(lexicalTarget, ec);
    if(ec)
        return false;
    const fs::path relative = target.lexically_relative(root);
    if(relative.empty())
        return false;
    for(const fs::path &part : relative) {
        if(part == fs::path(".."))
            return false;
    }

    resolvedPath = target.string();
    return true;
}

static bool EnsureArchiveDirectory(const std::string &rootPath,
                                   const std::string &entryName) {
    namespace fs = std::filesystem;
    std::string resolvedPath;
    if (entryName.empty()) {
        std::error_code ec;
        resolvedPath = fs::weakly_canonical(
                           fs::absolute(fs::u8path(rootPath), ec), ec)
                           .string();
        if (ec || resolvedPath.empty())
            return false;
    } else if (!ResolveArchivePath(rootPath, entryName, resolvedPath)) {
        return false;
    }

#if defined(_WIN32)
    std::error_code ec;
    fs::create_directories(fs::u8path(resolvedPath), ec);
    return !ec && fs::is_directory(fs::u8path(resolvedPath), ec) && !ec;
#else
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(
        fs::absolute(fs::u8path(rootPath), ec), ec);
    if (ec)
        return false;
    fs::create_directories(root, ec);
    if (ec)
        return false;

    const fs::path relative = entryName.empty()
                                  ? fs::path()
                                  : fs::u8path(entryName).lexically_normal();
    std::vector<std::string> components;
    for (const fs::path &part : relative) {
        if (part == fs::path("."))
            continue;
        if (part.empty() || part == fs::path(".."))
            return false;
        components.push_back(part.string());
    }

    int dir_flags = O_RDONLY;
#ifdef O_DIRECTORY
    dir_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    dir_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    const int nofollow = O_NOFOLLOW;
#else
    const int nofollow = 0;
#endif

    int dir_fd = open(root.c_str(), dir_flags | nofollow);
    if (dir_fd < 0)
        return false;
    for (const std::string &component : components) {
        int next_fd = openat(dir_fd, component.c_str(),
                             dir_flags | nofollow);
        if (next_fd < 0 && errno == ENOENT) {
            if (mkdirat(dir_fd, component.c_str(), 0755) != 0 &&
                errno != EEXIST) {
                close(dir_fd);
                return false;
            }
            next_fd = openat(dir_fd, component.c_str(),
                             dir_flags | nofollow);
        }
        if (next_fd < 0) {
            close(dir_fd);
            return false;
        }
        close(dir_fd);
        dir_fd = next_fd;
    }
    close(dir_fd);
    return true;
#endif
}

static FILE *OpenArchiveOutputFile(const std::string &rootPath,
                                   const std::string &entryName,
                                   std::string &resolvedPath) {
    if (!ResolveArchivePath(rootPath, entryName, resolvedPath))
        return nullptr;

#if defined(_WIN32)
    return _fileopen(ttstr(resolvedPath.c_str()));
#else
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(
        fs::absolute(fs::u8path(rootPath), ec), ec);
    if (ec)
        return nullptr;
    fs::create_directories(root, ec);
    if (ec)
        return nullptr;

    const fs::path relative = fs::u8path(entryName).lexically_normal();
    std::vector<std::string> components;
    for (const fs::path &part : relative) {
        if (part == fs::path("."))
            continue;
        if (part.empty() || part == fs::path(".."))
            return nullptr;
        components.push_back(part.string());
    }
    if (components.empty())
        return nullptr;

    int dir_flags = O_RDONLY;
#ifdef O_DIRECTORY
    dir_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    dir_flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    const int nofollow = O_NOFOLLOW;
#else
    const int nofollow = 0;
#endif

    int dir_fd = open(root.c_str(), dir_flags | nofollow);
    if (dir_fd < 0)
        return nullptr;
    for (size_t i = 0; i + 1 < components.size(); ++i) {
        int next_fd = openat(dir_fd, components[i].c_str(),
                             dir_flags | nofollow);
        if (next_fd < 0 && errno == ENOENT) {
            if (mkdirat(dir_fd, components[i].c_str(), 0755) != 0 &&
                errno != EEXIST) {
                close(dir_fd);
                return nullptr;
            }
            next_fd = openat(dir_fd, components[i].c_str(),
                             dir_flags | nofollow);
        }
        if (next_fd < 0) {
            close(dir_fd);
            return nullptr;
        }
        close(dir_fd);
        dir_fd = next_fd;
    }

    int file_flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    file_flags |= O_CLOEXEC;
#endif
    file_flags |= nofollow;
    const int file_fd = openat(dir_fd, components.back().c_str(),
                               file_flags, 0666);
    close(dir_fd);
    if (file_fd < 0)
        return nullptr;
    FILE *file = fdopen(file_fd, "wb");
    if (!file)
        close(file_fd);
    return file;
#endif
}

class tTVPUnpackArchiveThread {
    std::thread *ThreadObj;
    std::mutex Mutex;
    std::condition_variable Cond;
    bool Started = false;
    bool Cancelled = false;
    tTVPUnpackArchive *Owner;

    void Entry() {
        std::unique_lock<std::mutex> lk(Mutex);
        Cond.wait(lk, [this] { return Started || Cancelled; });
        if(Cancelled)
            return;
        lk.unlock();
        try {
            Owner->Process();
        } catch(...) {
            try {
                if(Owner->FuncOnError)
                    Owner->FuncOnError(ARCHIVE_FAILED,
                                       "Archive extraction failed");
            } catch(...) {
            }
        }
    }

public:
    tTVPUnpackArchiveThread(tTVPUnpackArchive *owner) : Owner(owner) {
        ThreadObj = new std::thread(&tTVPUnpackArchiveThread::Entry, this);
    }

    ~tTVPUnpackArchiveThread() {
        Cancel();
        if(ThreadObj->joinable()) {
            ThreadObj->join();
        }
        delete ThreadObj;
    }

    void Start() {
        {
            std::lock_guard<std::mutex> lk(Mutex);
            if(Cancelled)
                return;
            Started = true;
        }
        Cond.notify_all();
    }

    void Cancel() {
        {
            std::lock_guard<std::mutex> lk(Mutex);
            Cancelled = true;
        }
        Cond.notify_all();
    }
};

class tTVPUnpackArchiveImplLibArchive : public iTVPUnpackArchiveImpl {
    struct archive *ArcObj = nullptr;
    tTVPArchive *pTVPArc = nullptr;
    tjs_int64 _totalSize = 0;
    int _totalFileCount = 0;
    FILE *FpIn = nullptr;
    std::string _passphrase;

    static const char *_onPassphraseCallback(struct archive *,
                                             void *clientdata);

    std::string onPassphraseCallback();

public:
    ~tTVPUnpackArchiveImplLibArchive() override {
        if(pTVPArc)
            pTVPArc->Release();
        if(FpIn) {
            fclose(FpIn);
            FpIn = nullptr;
        }
        if(ArcObj) {
            archive_read_free(ArcObj);
            ArcObj = nullptr;
        }
    }

    bool Open(const std::string &path) override {
        FpIn = fopen(path.c_str(), "rb");
        if(!FpIn)
            return false;
        int file_count = 0;
        tjs_uint64 size_count = 0;
        ArcObj = archive_read_new();
        if(!ArcObj) {
            fclose(FpIn);
            FpIn = nullptr;
            return false;
        }
        archive_read_support_filter_all(ArcObj);
        archive_read_support_format_all(ArcObj);
        archive_read_set_passphrase_callback(ArcObj, this,
                                             _onPassphraseCallback);
        int r = archive_read_open_FILE(ArcObj, FpIn);

        if(r < ARCHIVE_OK) {
            fclose(FpIn);
            FpIn = nullptr;
            archive_read_free(ArcObj);
            ArcObj = nullptr;
            // try TVPArchive
            pTVPArc = TVPOpenArchive(path, false);
            if(pTVPArc) {
                file_count = pTVPArc->GetCount();
                if(file_count < 0)
                    return false;
                _totalSize = 0;
                for(int i = 0; i < file_count; ++i) {
                    tTJSBinaryStream *str = pTVPArc->CreateStreamByIndex(i);
                    if(str) {
                        const tjs_uint64 stream_size = str->GetSize();
                        if(stream_size >
                           static_cast<tjs_uint64>(
                               std::numeric_limits<tjs_int64>::max()) -
                               static_cast<tjs_uint64>(_totalSize)) {
                            delete str;
                            return false;
                        }
                        _totalSize += static_cast<tjs_int64>(stream_size);
                        delete str;
                    }
                }
                _totalFileCount = file_count;
                return true;
            }
            return false;
        }
        if(archive_read_has_encrypted_entries(ArcObj) > 0) {
            if(_passphrase.empty())
                _passphrase = onPassphraseCallback();
            if(_passphrase.empty()) {
                return false;
            }
            if(archive_read_add_passphrase(ArcObj, _passphrase.c_str()) <
               ARCHIVE_OK)
                return false;
        }
        bool header_failed = false;
        while(true) {
            struct archive_entry *entry;
            r = archive_read_next_header(ArcObj, &entry);
            if(r == ARCHIVE_EOF) {
                break;
            }
            if(r < ARCHIVE_OK) {
                NotifyError(r, archive_error_string(ArcObj));
                header_failed = true;
                break;
            }
            ++file_count;
            const la_int64_t entry_size = archive_entry_size(entry);
            if(entry_size > 0) {
                const auto unsigned_size = static_cast<tjs_uint64>(entry_size);
                if(unsigned_size >
                   static_cast<tjs_uint64>(std::numeric_limits<tjs_int64>::max()) -
                       size_count)
                    return false;
                size_count += unsigned_size;
            }
        }

        if(header_failed) {
            archive_read_close(ArcObj);
            archive_read_free(ArcObj);
            ArcObj = nullptr;
            fclose(FpIn);
            FpIn = nullptr;
            return false;
        }

        _totalSize = size_count;
        _totalFileCount = file_count;
        archive_read_close(ArcObj);
        archive_read_free(ArcObj);
        ArcObj = nullptr;
        return true;
    }

    int GetFileCount() override { return _totalFileCount; }

    tjs_int64 GetTotalSize() override { return _totalSize; }

    void ExtractTo(const std::string &OutPath) override {
        struct EndGuard {
            tTVPUnpackArchiveImplLibArchive *owner;
            ~EndGuard() { owner->NotifyEnded(); }
        } end_guard{this};
        tjs_uint64 total_size = 0;
        if(pTVPArc) {
            int file_count = pTVPArc->GetCount();
            std::vector<char> buffer;
            buffer.resize(4 * 1024 * 1024);
            bool failed = false;
            for(int index = 0; index < file_count && !StopRequired; ++index) {
                tjs_uint64 file_size = 0;
                std::string filename = pTVPArc->GetName(index).AsStdString();
                if(filename.size() > 600)
                    continue;
                std::string fullpath;
                FILE *fp = OpenArchiveOutputFile(OutPath, filename, fullpath);
                if(!fp) {
                    NotifyError(ARCHIVE_FAILED,
                                "Unsafe archive path or cannot open output file");
                    failed = true;
                    break;
                }
                tTJSBinaryStream *str = pTVPArc->CreateStreamByIndex(index);
                if(!str) {
                    NotifyError(ARCHIVE_FAILED, "Cannot open archive stream");
                    fclose(fp);
                    remove(fullpath.c_str());
                    failed = true;
                    break;
                }
                const tjs_uint64 expected_size = str->GetSize();
                NotifyNewFile(index, filename.c_str(), expected_size);
                while(!StopRequired) {
                    tjs_uint readed = str->Read(&buffer.front(), buffer.size());
                    if(readed == 0)
                        break;
                    if(readed != fwrite(&buffer.front(), 1, readed, fp)) {
                        NotifyError(ARCHIVE_FAILED,
                                    "Fail to write file.\nPlease check the "
                                    "disk space.");
                        failed = true;
                        break;
                    }
                    if(file_size > std::numeric_limits<tjs_uint64>::max() -
                                       readed ||
                       total_size > std::numeric_limits<tjs_uint64>::max() -
                                        readed) {
                        NotifyError(ARCHIVE_FAILED, "Archive size overflow");
                        failed = true;
                        break;
                    }
                    file_size += readed;
                    total_size += readed;
                    NotifyProgress(total_size, file_size);
                    if(readed < buffer.size())
                        break;
                }
                delete str;
                if(fclose(fp) != 0 && !failed) {
                    NotifyError(ARCHIVE_FAILED,
                                "Fail to close output file.\nPlease check "
                                "the disk space.");
                    failed = true;
                }
                if(StopRequired || file_size != expected_size) {
                    if(!StopRequired && !failed)
                        NotifyError(ARCHIVE_FAILED, "Unexpected end of archive stream");
                    failed = true;
                }
                if(failed) {
                    remove(fullpath.c_str());
                    break;
                }
            }
            pTVPArc->Release();
            pTVPArc = nullptr;
            return;
        }

        if (fseek(FpIn, 0, SEEK_SET) != 0) {
            NotifyError(ARCHIVE_FAILED, "Cannot seek archive input");
            return;
        }
        ArcObj = archive_read_new();
        if(!ArcObj) {
            NotifyError(ARCHIVE_FAILED, "Cannot allocate archive reader");
            return;
        }
        archive_read_support_filter_all(ArcObj);
        archive_read_support_format_all(ArcObj);
        archive_read_set_passphrase_callback(ArcObj, this,
                                             _onPassphraseCallback);
        if (!_passphrase.empty() &&
            archive_read_add_passphrase(ArcObj, _passphrase.c_str()) <
                ARCHIVE_OK) {
            NotifyError(ARCHIVE_FAILED, "Cannot set archive password");
            archive_read_free(ArcObj);
            ArcObj = nullptr;
            return;
        }
        int r = archive_read_open_FILE(ArcObj, FpIn);
        if(r < ARCHIVE_OK) {
            NotifyError(r, archive_error_string(ArcObj));
            archive_read_free(ArcObj);
            ArcObj = nullptr;
            return;
        }
        for(int index = 0; !StopRequired; ++index) {
            struct archive_entry *entry;
            r = archive_read_next_header(ArcObj, &entry);
            if(r == ARCHIVE_EOF) {
                break;
            }
            if(r < ARCHIVE_OK) {
                NotifyError(r, archive_error_string(ArcObj));
                break;
            }
            const char *filename = archive_entry_pathname_utf8(entry);
            if(!filename) {
                NotifyError(ARCHIVE_FAILED, "Archive entry has no path");
                break;
            }
            const auto file_type = archive_entry_filetype(entry);
            std::string fullpath;
            if(file_type == AE_IFDIR) {
                if(!ResolveArchivePath(OutPath, filename, fullpath)) {
                    NotifyError(ARCHIVE_FAILED, "Unsafe archive path");
                    break;
                }
                if(!EnsureArchiveDirectory(OutPath, filename)) {
                    NotifyError(ARCHIVE_FAILED,
                                "Cannot create output directory");
                    break;
                }
                continue;
            }
            if(file_type != AE_IFREG && file_type != 0) {
                NotifyError(ARCHIVE_FAILED,
                            "Unsupported archive entry type");
                break;
            }
            FILE *fp = OpenArchiveOutputFile(OutPath, filename, fullpath);
            if(!fp) {
                NotifyError(ARCHIVE_FAILED,
                            "Unsafe archive path or cannot open output file");
                break;
            }
            const la_int64_t entry_size = archive_entry_size(entry);
            NotifyNewFile(index, filename,
                          entry_size >= 0 ? static_cast<tjs_uint64>(entry_size)
                                          : 0);

            const void *buff;
            size_t size;
            la_int64_t offset;
            tjs_uint64 file_size = 0;
            const char *errmsg = nullptr;
            bool failed = false;
            while(!StopRequired) {
                r = archive_read_data_block(ArcObj, &buff, &size, &offset);
                if(r == ARCHIVE_EOF) {
                    r = ARCHIVE_OK;
                    break;
                }
                if(r < ARCHIVE_OK) {
                    errmsg = archive_error_string(ArcObj);
                    failed = true;
                    break;
                }
                if(offset < 0 || static_cast<tjs_uint64>(offset) != file_size) {
                    r = ARCHIVE_FAILED;
                    errmsg = "Unsupported sparse or non-contiguous archive entry";
                    failed = true;
                    break;
                }
                if(size != fwrite(buff, 1, size, fp)) {
                    r = ARCHIVE_FAILED;
                    errmsg = "Fail to write file.\nPlease check the "
                             "disk space.";
                    failed = true;
                    break;
                }
                if(file_size > std::numeric_limits<tjs_uint64>::max() - size ||
                   total_size > std::numeric_limits<tjs_uint64>::max() - size) {
                    r = ARCHIVE_FAILED;
                    errmsg = "Archive size overflow";
                    failed = true;
                    break;
                }
                file_size += size;
                total_size += size;
                NotifyProgress(total_size, file_size);
            }
            if(!failed && !StopRequired && entry_size >= 0 &&
               file_size != static_cast<tjs_uint64>(entry_size)) {
                r = ARCHIVE_FAILED;
                errmsg = "Unexpected end of archive entry";
                failed = true;
            }
            if(fclose(fp) != 0 && !failed) {
                r = ARCHIVE_FAILED;
                errmsg = "Fail to close output file.\nPlease check the disk space.";
                failed = true;
            }
            if(failed || StopRequired)
                remove(fullpath.c_str());
            if(r < ARCHIVE_OK)
                NotifyError(r, errmsg);
            if(r < ARCHIVE_WARN)
                break;
            if(!failed && !StopRequired && archive_entry_mtime_is_set(entry)) {
                TVP_utime(fullpath.c_str(), archive_entry_mtime(entry));
            }
        }
        archive_read_close(ArcObj);
        archive_read_free(ArcObj);
        ArcObj = nullptr;
    }
};

std::string tTVPUnpackArchiveImplLibArchive::onPassphraseCallback() {
    return RequestPassword();
}

const char *
tTVPUnpackArchiveImplLibArchive::_onPassphraseCallback(archive *,
                                                       void *clientdata) {
    auto *impl = static_cast<tTVPUnpackArchiveImplLibArchive *>(clientdata);
    impl->_passphrase = impl->onPassphraseCallback();
    return impl->_passphrase.empty() ? nullptr : impl->_passphrase.c_str();
}

#if 1

#include <raros.hpp>
#include <dll.hpp>

class tTVPUnpackArchiveImplUnRAR : public iTVPUnpackArchiveImpl {
    std::string _archivePath;
    std::vector<std::string> _filelist;
    tjs_int64 _totalSize, _totalProcessedBytes, _curProcessedBytes;
    std::string _lastUsedPassword;
    std::mutex _mutex;
    std::condition_variable _cond;

    bool _reqBreak = false;

    struct RARArc {
        RAROpenArchiveDataEx _archiveData;
        void *_handle = nullptr;

        RARArc() {}

        ~RARArc() { Close(); }

        bool Open(char *path, int mode) {
            memset(&_archiveData, 0, sizeof(_archiveData));
            _archiveData.ArcName = path;
            _archiveData.OpenMode = mode;
            _handle = RAROpenArchiveEx(&_archiveData);
            return !!_handle;
        }

        void Close() {
            if(_handle)
                RARCloseArchive(_handle);
            _handle = nullptr;
        }
    };

    int OnCallback(UINT msg, LPARAM P1, LPARAM P2) {
        switch(msg) {
            case UCM_CHANGEVOLUME:
            case UCM_CHANGEVOLUMEW:
                return -1; // manual change multi-volume file name is
                           // not supported yet
            case UCM_NEEDPASSWORD: {
                bool hasPsw = !_lastUsedPassword.empty();
                if(!hasPsw) {
                    _lastUsedPassword = RequestPassword();
                }
                if(_lastUsedPassword.empty() || P1 == 0 || P2 <= 0) {
                    return -1;
                }
                const size_t capacity = static_cast<size_t>(P2);
                const size_t len = std::min(_lastUsedPassword.size(),
                                            capacity - 1);
                memcpy(reinterpret_cast<char *>(P1), _lastUsedPassword.data(),
                       len);
                reinterpret_cast<char *>(P1)[len] = '\0';
                if(hasPsw)
                    _lastUsedPassword.clear();
                return 0;
            }
            case UCM_PROCESSDATA:
                if (P2 > 0) {
                    const auto processed = static_cast<tjs_uint64>(P2);
                    _totalProcessedBytes += processed;
                    _curProcessedBytes += processed;
                }
                NotifyProgress(static_cast<tjs_uint64>(_totalProcessedBytes),
                               static_cast<tjs_uint64>(_curProcessedBytes));
                return StopRequired.load() ? -1 : 0;
        }
        return -1;
    }

public:
    ~tTVPUnpackArchiveImplUnRAR() override {}

    bool Open(const std::string &path) override {
        _archivePath = path;
        RARArc arc;
        if(!arc.Open((char *)_archivePath.c_str(), RAR_OM_LIST)) {
            return false;
        }
        RARSetCallback(
            arc._handle,
            [](UINT msg, LPARAM UserData, LPARAM P1, LPARAM P2) -> int {
                return ((tTVPUnpackArchiveImplUnRAR *)UserData)
                    ->OnCallback(msg, P1, P2);
            },
            (LPARAM)this);

        RARHeaderData headerData;
        _totalSize = 0;
        _filelist.clear();
        while(1) {
            RARHeaderDataEx headerData;
            memset(&headerData, 0, sizeof(headerData));
            int result = RARReadHeaderEx(arc._handle, &headerData);
            if(result != 0) {
                if(result != ERAR_END_ARCHIVE) {
                    return false;
                }
                break;
            }

            const tjs_uint64 fileSize =
                (static_cast<tjs_uint64>(headerData.UnpSizeHigh) << 32) |
                static_cast<tjs_uint64>(headerData.UnpSize);
            if(fileSize > static_cast<tjs_uint64>(
                              std::numeric_limits<tjs_int64>::max()) -
                              static_cast<tjs_uint64>(_totalSize)) {
                return false;
            }
            _totalSize += static_cast<tjs_int64>(fileSize);
            _filelist.emplace_back(headerData.FileName);
            // Find next file
            result = RARProcessFile(arc._handle, RAR_SKIP, nullptr, nullptr);
            if(result != 0) {
                return false;
            }
        }
        return true;
    }

    int GetFileCount() override { return _filelist.size(); }

    tjs_int64 GetTotalSize() override { return _totalSize; }

    void ExtractTo(const std::string &path) override {
        struct EndGuard {
            tTVPUnpackArchiveImplUnRAR *owner;
            ~EndGuard() { owner->NotifyEnded(); }
        } end_guard{this};
        RARArc arc;
        if(!arc.Open((char *)_archivePath.c_str(), RAR_OM_EXTRACT)) {
            NotifyError(1001, "Cannot open file");
            return;
        }
        RARSetCallback(
            arc._handle,
            [](UINT msg, LPARAM UserData, LPARAM P1, LPARAM P2) -> int {
                return ((tTVPUnpackArchiveImplUnRAR *)UserData)
                    ->OnCallback(msg, P1, P2);
            },
            (LPARAM)this);
        for(int counter = 0;; ++counter) {
            RARHeaderDataEx headerData;
            memset(&headerData, 0, sizeof(headerData));
            int result = RARReadHeaderEx(arc._handle, &headerData);
            if(result != 0) {
                if(result != ERAR_END_ARCHIVE) {
                    NotifyError(result, "Extraction Fail");
                    return;
                }
                break;
            }

            // _filelist.emplace_back(headerData.FileName);
            const tjs_uint64 fileSize =
                (static_cast<tjs_uint64>(headerData.UnpSizeHigh) << 32) |
                static_cast<tjs_uint64>(headerData.UnpSize);
            NotifyNewFile(counter, headerData.FileName, fileSize);
            _curProcessedBytes = 0;
            std::string safePath;
            if(!ResolveArchivePath(path, headerData.FileName, safePath)) {
                NotifyError(1001, "Unsafe archive path");
                return;
            }
            namespace fs = std::filesystem;
            const fs::path safeFile = fs::u8path(safePath);
            const fs::path safeParent = safeFile.parent_path();
            const fs::path entryPath = fs::u8path(headerData.FileName);
            if(!EnsureArchiveDirectory(path, entryPath.parent_path().string())) {
                NotifyError(1001, "Cannot create archive path");
                return;
            }
            std::string destinationPath = safeParent.string();
            std::string destinationName = safeFile.filename().string();
            // Find next file
            result = RARProcessFile(arc._handle, RAR_EXTRACT,
                                    destinationPath.empty()
                                        ? nullptr
                                        : (char *)destinationPath.c_str(),
                                    (char *)destinationName.c_str());
            if(result != 0) {
                NotifyError(result, "Extraction Fail");
                return;
            }
        }
    }
};

#endif

int tTVPUnpackArchive::Prepare(const std::string &path,
                               const std::string &_outpath,
                               tjs_uint64 *totalSize) {
    Close();
    FILE *FpIn = fopen(path.c_str(), "rb");
    if(!FpIn)
        return -1;
    char signature[4];
    if(fread(signature, 1, 4, FpIn) != 4) {
        fclose(FpIn);
        return -2;
    }
    OutPath = _outpath + "/";
    fclose(FpIn);
#ifdef _UNRAR_DLL_
    if(!memcmp(signature, "Rar!", 4)) {
        _impl = new tTVPUnpackArchiveImplUnRAR();
    } else
#endif
    {
        _impl = new tTVPUnpackArchiveImplLibArchive();
    }
    _impl->SetCallback(this);
    if(!_impl->Open(path)) {
        Close();
        return -2;
    }
    if(totalSize)
        *totalSize = _impl->GetTotalSize();
    int file_count = _impl->GetFileCount();
    if(file_count) {
        ArcThread = new tTVPUnpackArchiveThread(this);
    }
    return file_count;
}

void tTVPUnpackArchive::Start() {
    if(!_impl || !ArcThread)
        return;
    _impl->StopRequired.store(false);
    ArcThread->Start();
}

void tTVPUnpackArchive::Stop() {
    if(!_impl)
        return;
    _impl->StopRequired.store(true);
    if(ArcThread)
        ArcThread->Cancel();
}

void tTVPUnpackArchive::Close() {
    if(_impl)
        _impl->StopRequired.store(true);
    if(ArcThread) {
        ArcThread->Cancel();
        delete ArcThread;
        ArcThread = nullptr;
    }
    if(_impl)
        delete _impl;
    _impl = nullptr;
}

void tTVPUnpackArchive::Process() {
    if(!_impl || _impl->StopRequired.load())
        return;
    _impl->ExtractTo(OutPath);
}

tTVPUnpackArchive::tTVPUnpackArchive() {}

tTVPUnpackArchive::~tTVPUnpackArchive() {
    Close();
}
