// SPDX-License-Identifier: GPL-2.0-or-later

// included header files
#include "config.h"
#include "datasets.hpp"
#include "basicio.hpp"
#include "futils.hpp"
#include "types.hpp"
#include "error.hpp"
#include "enforce.hpp"
#include "http.hpp"
#include "image_int.hpp"

// + standard includes
#include <fcntl.h>      // _O_BINARY in FileIo::FileIo
#include <sys/stat.h>   // for stat, chmod
#include <sys/types.h>  // for stat, chmod

#include <cassert>
#include <cstdio>   // for remove, rename
#include <cstdlib>  // for alloc, realloc, free
#include <cstring>  // std::memcpy
#include <ctime>    // timestamp for the name of temporary file
#include <filesystem>
#include <fstream>  // write the temporary file
#include <iostream>
#include <memory>
#include <string>

#ifdef EXV_HAVE_SYS_MMAN_H
# include <sys/mman.h>                  // for mmap and munmap
#endif
#ifdef EXV_HAVE_PROCESS_H
# include <process.h>
#endif
#ifdef EXV_HAVE_UNISTD_H
# include <unistd.h>                    // for getpid, stat
#endif

#ifdef EXV_USE_CURL
# include <curl/curl.h>
#endif

#if defined(__MINGW__) || (defined(WIN32) && !defined(__CYGWIN__))
#define mode_t unsigned short
# include <windows.h>
# include <io.h>
#endif

namespace fs = std::filesystem;

// *****************************************************************************
// class member definitions
namespace {
    /// @brief replace each substring of the subject that matches the given search string with the given replacement.
    void ReplaceStringInPlace(std::string& subject, const std::string& search, const std::string& replace)
    {
        auto pos = subject.find(search);
        while (pos != std::string::npos) {
            subject.replace(pos, search.length(), replace);
            pos += subject.find(search, pos + replace.length());
        }
    }
}

namespace Exiv2 {
    void BasicIo::readOrThrow(byte* buf, size_t rcount, ErrorCode err) {
        const size_t nread = read(buf, rcount);
        enforce(nread == rcount, err);
        enforce(!error(), err);
    }

    void BasicIo::seekOrThrow(int64_t offset, Position pos, ErrorCode err) {
        const int r = seek(offset, pos);
        enforce(r == 0, err);
    }

    //! Internal Pimpl structure of class FileIo.
    class FileIo::Impl {
    public:
        //! Constructor
        explicit Impl(std::string path);
        // Enumerations
        //! Mode of operation
        enum OpMode { opRead, opWrite, opSeek };
        // DATA
        std::string path_;              //!< (Standard) path
        std::string openMode_;          //!< File open mode
        FILE* fp_{};                    //!< File stream pointer
        OpMode opMode_{opSeek};         //!< File open mode

#if defined WIN32 && !defined __CYGWIN__
        HANDLE hFile_{};  //!< Duplicated fd
        HANDLE hMap_{};   //!< Handle from CreateFileMapping
#endif
        byte* pMappedArea_{};    //!< Pointer to the memory-mapped area
        size_t mappedLength_{};  //!< Size of the memory-mapped area
        bool isMalloced_{};      //!< Is the mapped area allocated?
        bool isWriteable_{};     //!< Can the mapped area be written to?
        // TYPES
        //! Simple struct stat wrapper for internal use
        struct StructStat {
            StructStat() = default;
            mode_t st_mode{0};    //!< Permissions
            off_t st_size{0};     //!< Size
        };
// #endif
        // METHODS
        /*!
          @brief Switch to a new access mode, reopening the file if needed.
              Optimized to only reopen the file when it is really necessary.
          @param opMode The mode to switch to.
          @return 0 if successful
         */
        int switchMode(OpMode opMode);
        //! stat wrapper for internal use
        int stat(StructStat& buf) const;
        // NOT IMPLEMENTED
        Impl(const Impl& rhs) = delete;             //!< Copy constructor
        Impl& operator=(const Impl& rhs) = delete;  //!< Assignment
    }; // class FileIo::Impl

    FileIo::Impl::Impl(std::string path) : path_(std::move(path))
    {
    }

    int FileIo::Impl::switchMode(OpMode opMode)
    {
        assert(fp_ != 0);
        if (opMode_ == opMode) return 0;
        OpMode oldOpMode = opMode_;
        opMode_ = opMode;

        bool reopen = true;
        switch(opMode) {
        case opRead:
            // Flush if current mode allows reading, else reopen (in mode "r+b"
            // as in this case we know that we can write to the file)
            if (openMode_.at(0) == 'r' || openMode_.at(1) == '+') reopen = false;
            break;
        case opWrite:
            // Flush if current mode allows writing, else reopen
            if (openMode_.at(0) != 'r' || openMode_.at(1) == '+') reopen = false;
            break;
        case opSeek:
            reopen = false;
            break;
        }

        if (!reopen) {
            // Don't do anything when switching _from_ opSeek mode; we
            // flush when switching _to_ opSeek.
            if (oldOpMode == opSeek) return 0;

            // Flush. On msvcrt fflush does not do the job
            std::fseek(fp_, 0, SEEK_CUR);
            return 0;
        }

        // Reopen the file
        long offset = std::ftell(fp_);
        if (offset == -1) return -1;
        // 'Manual' open("r+b") to avoid munmap()
        if (fp_ != nullptr) {
            std::fclose(fp_);
            fp_ = nullptr;
        }
        openMode_ = "r+b";
        opMode_ = opSeek;
        fp_ = std::fopen(path_.c_str(), openMode_.c_str());
        if (!fp_) return 1;
        return std::fseek(fp_, offset, SEEK_SET);
    } // FileIo::Impl::switchMode

    int FileIo::Impl::stat(StructStat& buf) const
    {
        int ret = 0;
        struct stat st;
        ret = ::stat(path_.c_str(), &st);
        if (0 == ret) {
            buf.st_size = st.st_size;
            buf.st_mode = st.st_mode;
        }
        return ret;
    } // FileIo::Impl::stat

    FileIo::FileIo(const std::string& path)
        : p_(std::make_unique<Impl>(path))
    {
    }

    FileIo::~FileIo()
    {
        close();
    }

    int FileIo::munmap()
    {
        int rc = 0;
        if (p_->pMappedArea_ != nullptr) {
#if defined EXV_HAVE_MMAP && defined EXV_HAVE_MUNMAP
            if (::munmap(p_->pMappedArea_, p_->mappedLength_) != 0) {
                rc = 1;
            }
#elif defined WIN32 && !defined __CYGWIN__
            UnmapViewOfFile(p_->pMappedArea_);
            CloseHandle(p_->hMap_);
            p_->hMap_ = 0;
            CloseHandle(p_->hFile_);
            p_->hFile_ = 0;
#else
            if (p_->isWriteable_) {
                seek(0, BasicIo::beg);
                write(p_->pMappedArea_, p_->mappedLength_);
            }
            if (p_->isMalloced_) {
                delete[] p_->pMappedArea_;
                p_->isMalloced_ = false;
            }
#endif
        }
        if (p_->isWriteable_) {
            if (p_->fp_ != nullptr)
                p_->switchMode(Impl::opRead);
            p_->isWriteable_ = false;
        }
        p_->pMappedArea_ = nullptr;
        p_->mappedLength_ = 0;
        return rc;
    }

    byte* FileIo::mmap(bool isWriteable)
    {
        assert(p_->fp_ != 0);
        if (munmap() != 0) {
            throw Error(kerCallFailed, path(), strError(), "munmap");
        }
        p_->mappedLength_ = size();
        p_->isWriteable_ = isWriteable;
        if (p_->isWriteable_ && p_->switchMode(Impl::opWrite) != 0) {
            throw Error(kerFailedToMapFileForReadWrite, path(), strError());
        }
#if defined EXV_HAVE_MMAP && defined EXV_HAVE_MUNMAP
        int prot = PROT_READ;
        if (p_->isWriteable_) {
            prot |= PROT_WRITE;
        }
        void* rc = ::mmap(nullptr, p_->mappedLength_, prot, MAP_SHARED, fileno(p_->fp_), 0);
        if (MAP_FAILED == rc) {
            throw Error(kerCallFailed, path(), strError(), "mmap");
        }
        p_->pMappedArea_ = static_cast<byte*>(rc);

#elif defined WIN32 && !defined __CYGWIN__
        // Windows implementation

        // TODO: An attempt to map a file with a length of 0 (zero) fails with
        // an error code of ERROR_FILE_INVALID.
        // Applications should test for files with a length of 0 (zero) and
        // reject those files.

        DWORD dwAccess = FILE_MAP_READ;
        DWORD flProtect = PAGE_READONLY;
        if (isWriteable) {
            dwAccess = FILE_MAP_WRITE;
            flProtect = PAGE_READWRITE;
        }
        HANDLE hPh = GetCurrentProcess();
        HANDLE hFd = (HANDLE)_get_osfhandle(fileno(p_->fp_));
        if (hFd == INVALID_HANDLE_VALUE) {
            throw Error(kerCallFailed, path(), "MSG1", "_get_osfhandle");
        }
        if (!DuplicateHandle(hPh, hFd, hPh, &p_->hFile_, 0, false, DUPLICATE_SAME_ACCESS)) {
            throw Error(kerCallFailed, path(), "MSG2", "DuplicateHandle");
        }
        p_->hMap_ = CreateFileMapping(p_->hFile_, 0, flProtect, 0, (DWORD) p_->mappedLength_, 0);
        if (p_->hMap_ == 0 ) {
            throw Error(kerCallFailed, path(), "MSG3", "CreateFileMapping");
        }
        void* rc = MapViewOfFile(p_->hMap_, dwAccess, 0, 0, 0);
        if (rc == 0) {
            throw Error(kerCallFailed, path(), "MSG4", "CreateFileMapping");
        }
        p_->pMappedArea_ = static_cast<byte*>(rc);
#else
        // Workaround for platforms without mmap: Read the file into memory
        DataBuf buf(static_cast<long>(p_->mappedLength_));
        if (read(buf.data(), buf.size()) != buf.size()) {
            throw Error(kerCallFailed, path(), strError(), "FileIo::read");
        }
        if (error()) {
            throw Error(kerCallFailed, path(), strError(), "FileIo::mmap");
        }
        p_->pMappedArea_ = buf.release().first;
        p_->isMalloced_ = true;
#endif
        return p_->pMappedArea_;
    }

    void FileIo::setPath(const std::string& path) {
        close();
        p_->path_ = path;
    }

    size_t FileIo::write(const byte* data, size_t wcount)
    {
        assert(p_->fp_ != 0);
        if (p_->switchMode(Impl::opWrite) != 0)
            return 0;
        return std::fwrite(data, 1, wcount, p_->fp_);
    }

    size_t FileIo::write(BasicIo& src)
    {
        assert(p_->fp_ != 0);
        if (static_cast<BasicIo*>(this) == &src)
            return 0;
        if (!src.isopen())
            return 0;
        if (p_->switchMode(Impl::opWrite) != 0)
            return 0;

        byte buf[4096];
        size_t readCount = 0;
        size_t writeTotal = 0;
        while ((readCount = src.read(buf, sizeof(buf)))) {
            size_t writeCount = std::fwrite(buf, 1, readCount, p_->fp_);
            writeTotal += writeCount;
            if (writeCount != readCount) {
                // try to reset back to where write stopped
                src.seek(writeCount-readCount, BasicIo::cur);
                break;
            }
        }

        return writeTotal;
    }

    void FileIo::transfer(BasicIo& src)
    {
        const bool wasOpen = (p_->fp_ != nullptr);
        const std::string lastMode(p_->openMode_);

        auto fileIo = dynamic_cast<FileIo*>(&src);
        if (fileIo) {
            // Optimization if src is another instance of FileIo
            fileIo->close();
            // Check if the file can be written to, if it already exists
            if (open("a+b") != 0) {
                // Remove the (temporary) file
                fs::remove(fileIo->path().c_str());
                throw Error(kerFileOpenFailed, path(), "a+b", strError());
            }
            close();

            bool statOk = true;
            mode_t origStMode = 0;
            auto pf = path().c_str();

            Impl::StructStat buf1;
            if (p_->stat(buf1) == -1) {
                statOk = false;
            }
            origStMode = buf1.st_mode;

            {
#if defined(WIN32) && defined(REPLACEFILE_IGNORE_MERGE_ERRORS)
                // Windows implementation that deals with the fact that ::rename fails
                // if the target filename still exists, which regularly happens when
                // that file has been opened with FILE_SHARE_DELETE by another process,
                // like a virus scanner or disk indexer
                // (see also http://stackoverflow.com/a/11023068)
                using ReplaceFileA_t = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, DWORD, LPVOID, LPVOID);
                HMODULE hKernel = ::GetModuleHandleA("kernel32.dll");
                if (hKernel) {
                    ReplaceFileA_t pfcn_ReplaceFileA = (ReplaceFileA_t)GetProcAddress(hKernel, "ReplaceFileA");
                    if (pfcn_ReplaceFileA) {
                        BOOL ret = pfcn_ReplaceFileA(pf, fileIo->path().c_str(), NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
                        if (ret == 0) {
                            if (GetLastError() == ERROR_FILE_NOT_FOUND) {
                                fs::rename(fileIo->path().c_str(), pf);
                                fs::remove(fileIo->path().c_str());
                            }
                            else {
                                throw Error(kerFileRenameFailed, fileIo->path(), pf, strError());
                            }
                        }
                    }
                    else {
                        if (fileExists(pf) && ::remove(pf) != 0) {
                            throw Error(kerCallFailed, pf, strError(), "fs::remove");
                        }
                        fs::rename(fileIo->path().c_str(), pf);
                        fs::remove(fileIo->path().c_str());
                    }
                }
#else
                if (fileExists(pf) && fs::remove(pf) != 0) {
                    throw Error(kerCallFailed, pf, strError(), "fs::remove");
                }
                fs::rename(fileIo->path().c_str(), pf);
                fs::remove(fileIo->path().c_str());
#endif
                // Check permissions of new file
                struct stat buf2;
                if (statOk && ::stat(pf, &buf2) == -1) {
                    statOk = false;
#ifndef SUPPRESS_WARNINGS
                    EXV_WARNING << Error(kerCallFailed, pf, strError(), "::stat") << "\n";
#endif
                }
                if (statOk && origStMode != buf2.st_mode) {
                    // Set original file permissions
                    if (::chmod(pf, origStMode) == -1) {
#ifndef SUPPRESS_WARNINGS
                        EXV_WARNING << Error(kerCallFailed, pf, strError(), "::chmod") << "\n";
#endif
                    }
                }
            }
        } // if (fileIo)
        else {
            // Generic handling, reopen both to reset to start
            if (open("w+b") != 0) {
                throw Error(kerFileOpenFailed, path(), "w+b", strError());
            }
            if (src.open() != 0) {
                throw Error(kerDataSourceOpenFailed, src.path(), strError());
            }
            write(src);
            src.close();
        }

        if (wasOpen) {
            if (open(lastMode) != 0) {
                throw Error(kerFileOpenFailed, path(), lastMode, strError());
            }
        }
        else close();

        if (error() || src.error()) {
            throw Error(kerTransferFailed, path(), strError());
        }
    } // FileIo::transfer

    int FileIo::putb(byte data)
    {
        assert(p_->fp_ != 0);
        if (p_->switchMode(Impl::opWrite) != 0) return EOF;
        return putc(data, p_->fp_);
    }

    int FileIo::seek( int64_t offset, Position pos )
    {
        assert(p_->fp_ != 0);

        int fileSeek = 0;
        switch (pos) {
        case BasicIo::cur: fileSeek = SEEK_CUR; break;
        case BasicIo::beg: fileSeek = SEEK_SET; break;
        case BasicIo::end: fileSeek = SEEK_END; break;
        }

        if (p_->switchMode(Impl::opSeek) != 0) return 1;
#ifdef _WIN64
        return _fseeki64(p_->fp_, offset, fileSeek);
#else
        return std::fseek(p_->fp_,static_cast<long>(offset), fileSeek);
#endif
    }

    long FileIo::tell() const
    {
        assert(p_->fp_ != 0);
        return std::ftell(p_->fp_);
    }

    size_t FileIo::size() const
    {
        // Flush and commit only if the file is open for writing
        if (p_->fp_ != nullptr && (p_->openMode_.at(0) != 'r' || p_->openMode_.at(1) == '+')) {
            std::fflush(p_->fp_);
#if defined WIN32 && !defined __CYGWIN__
            // This is required on msvcrt before stat after writing to a file
            _commit(_fileno(p_->fp_));
#endif
        }

        Impl::StructStat buf;
        int ret = p_->stat(buf);

        if (ret != 0) return -1;
        return buf.st_size;
    }

    int FileIo::open()
    {
        // Default open is in read-only binary mode
        return open("rb");
    }

    int FileIo::open(const std::string& mode)
    {
        close();
        p_->openMode_ = mode;
        p_->opMode_ = Impl::opSeek;
        p_->fp_ = ::fopen(path().c_str(), mode.c_str());
        if (!p_->fp_)
            return 1;
        return 0;
    }

    bool FileIo::isopen() const
    {
        return p_->fp_ != nullptr;
    }

    int FileIo::close()
    {
        int rc = 0;
        if (munmap() != 0)
            rc = 2;
        if (p_->fp_ != nullptr) {
            if (std::fclose(p_->fp_) != 0)
                rc |= 1;
            p_->fp_ = nullptr;
        }
        return rc;
    }

    DataBuf FileIo::read(size_t rcount)
    {
        assert(p_->fp_ != 0);
        if (static_cast<size_t>(rcount) > size())
            throw Error(kerInvalidMalloc);
        DataBuf buf(rcount);
        size_t readCount = read(buf.data(), buf.size());
        if (readCount == 0) {
            throw Error(kerInputDataReadFailed);
        }
        buf.resize(readCount);
        return buf;
    }

    size_t FileIo::read(byte* buf, size_t rcount)
    {
        assert(p_->fp_ != 0);
        if (p_->switchMode(Impl::opRead) != 0) {
            return 0;
        }
        return std::fread(buf, 1, rcount, p_->fp_);
    }

    int FileIo::getb()
    {
        assert(p_->fp_ != 0);
        if (p_->switchMode(Impl::opRead) != 0) return EOF;
        return getc(p_->fp_);
    }

    int FileIo::error() const
    {
        return p_->fp_ != nullptr ? ferror(p_->fp_) : 0;
    }

    bool FileIo::eof() const
    {
        return std::feof(p_->fp_) != 0;
    }

    const std::string& FileIo::path() const noexcept
    {
        return p_->path_;
    }

    void FileIo::populateFakeData() {

    }

    //! Internal Pimpl structure of class MemIo.
    class MemIo::Impl final{
    public:
        Impl() = default;                  //!< Default constructor
        Impl(const byte* data, size_t size); //!< Constructor 2

        // DATA
        byte* data_{nullptr};     //!< Pointer to the start of the memory area
        size_t idx_{0};           //!< Index into the memory area
        size_t size_{0};          //!< Size of the memory area
        size_t sizeAlloced_{0};   //!< Size of the allocated buffer
        bool isMalloced_{false};  //!< Was the buffer allocated?
        bool eof_{false};         //!< EOF indicator

        // METHODS
        void reserve(size_t wcount);         //!< Reserve memory

        // NOT IMPLEMENTED
        Impl(const Impl& rhs) = delete;             //!< Copy constructor
        Impl& operator=(const Impl& rhs) = delete;  //!< Assignment
    }; // class MemIo::Impl

    MemIo::Impl::Impl(const byte* data, size_t size) : data_(const_cast<byte*>(data)), size_(size)
    {
    }

    /*!
      @brief Utility class provides the block mapping to the part of data. This avoids allocating
            a single contiguous block of memory to the big data.
     */
    class EXIV2API BlockMap {
    public:
        //! the status of the block.
        enum    blockType_e {bNone, bKnown, bMemory};
        //! @name Creators
        //@{
        //! Default constructor. the init status of the block is bNone.
        BlockMap() = default;

        //! Destructor. Releases all managed memory.
        ~BlockMap()
        {
            delete [] data_;
        }

        //! @brief Populate the block.
        //! @param source The data populate to the block
        //! @param num The size of data
        void    populate (byte* source, size_t num)
        {
            assert(source != nullptr);
            size_ = num;
            data_ = new byte [size_];
            type_ = bMemory;
            std::memcpy(data_, source, size_);
        }

        /*!
          @brief Change the status to bKnow. bKnow blocks do not contain the data,
                but they keep the size of data. This avoids allocating memory for parts
                of the file that contain image-date (non-metadata/pixel data) which never change in exiv2.
          @param num The size of the data
         */
        void    markKnown(size_t num)
        {
            type_ = bKnown;
            size_ = num;
        }

        bool    isNone() const
        {
            return type_ == bNone;
        }

        bool    isKnown () const
        {
            return type_ == bKnown;
        }

        byte*   getData () const
        {
            return data_;
        }

        size_t  getSize () const
        {
            return size_;
        }

    private:
        blockType_e type_{bNone};
        byte* data_{nullptr};
        size_t size_{0};
    }; // class BlockMap

    void MemIo::Impl::reserve(size_t wcount)
    {
        const size_t need = wcount + idx_;
        size_t blockSize =     32*1024;   // 32768
        const size_t maxBlockSize = 4*1024*1024;

        if (!isMalloced_) {
            // Minimum size for 1st block
            size_t size  = std::max(blockSize * (1 + need / blockSize), size_);
            auto data = static_cast<byte*>(std::malloc(size));
            if (data == nullptr) {
                throw Error(kerMallocFailed);
            }
            if (data_ != nullptr) {
                std::memcpy(data, data_, size_);
            }
            data_ = data;
            sizeAlloced_ = size;
            isMalloced_ = true;
        }

        if (need > size_) {
            if (need > sizeAlloced_) {
                blockSize = 2*sizeAlloced_ ;
                if ( blockSize > maxBlockSize ) blockSize = maxBlockSize ;
                // Allocate in blocks
                size_t want      = blockSize * (1 + need / blockSize );
                data_ = static_cast<byte*>(std::realloc(data_, want));
                if (data_ == nullptr) {
                    throw Error(kerMallocFailed);
                }
                sizeAlloced_ = want;
            }
            size_ = need;
        }
    }

    MemIo::MemIo()
        : p_(std::make_unique<Impl>())
    {
    }

    MemIo::MemIo(const byte* data, size_t size)
        : p_(std::make_unique<Impl>(data, size))
    {
    }

    MemIo::~MemIo()
    {
        if (p_->isMalloced_) {
            std::free(p_->data_);
        }
    }

    size_t MemIo::write(const byte* data, size_t wcount)
    {
        p_->reserve(wcount);
        assert(p_->isMalloced_);
        if (data != nullptr) {
            std::memcpy(&p_->data_[p_->idx_], data, wcount);
        }
        p_->idx_ += wcount;
        return wcount;
    }

    void MemIo::transfer(BasicIo& src)
    {
        auto memIo = dynamic_cast<MemIo*>(&src);
        if (memIo) {
            // Optimization if src is another instance of MemIo
            if (p_->isMalloced_) {
                std::free(p_->data_);
            }
            p_->idx_ = 0;
            p_->data_ = memIo->p_->data_;
            p_->size_ = memIo->p_->size_;
            p_->isMalloced_ = memIo->p_->isMalloced_;
            memIo->p_->idx_ = 0;
            memIo->p_->data_ = nullptr;
            memIo->p_->size_ = 0;
            memIo->p_->isMalloced_ = false;
        }
        else {
            // Generic reopen to reset position to start
            if (src.open() != 0) {
                throw Error(kerDataSourceOpenFailed, src.path(), strError());
            }
            p_->idx_ = 0;
            write(src);
            src.close();
        }
        if (error() || src.error()) throw Error(kerMemoryTransferFailed, strError());
    }

    size_t MemIo::write(BasicIo& src)
    {
        if (static_cast<BasicIo*>(this) == &src) return 0;
        if (!src.isopen()) return 0;

        byte buf[4096];
        size_t readCount = 0;
        size_t writeTotal = 0;
        while ((readCount = src.read(buf, sizeof(buf)))) {
            write(buf, readCount);
            writeTotal += readCount;
        }

        return writeTotal;
    }

    int MemIo::putb(byte data)
    {
        p_->reserve(1);
        assert(p_->isMalloced_);
        p_->data_[p_->idx_++] = data;
        return data;
    }

    int MemIo::seek( int64_t offset, Position pos )
    {
        int64_t newIdx = 0;

        switch (pos) {
        case BasicIo::cur: newIdx = p_->idx_ + offset; break;
        case BasicIo::beg: newIdx = offset; break;
        case BasicIo::end: newIdx = p_->size_ + offset; break;
        }

        if (newIdx < 0)
            return 1;

        if (newIdx > static_cast<int64_t>(p_->size_)) {
            p_->eof_ = true;
            return 1;
        }

        p_->idx_ = static_cast<long>(newIdx);
        p_->eof_ = false;
        return 0;
    }

    byte* MemIo::mmap(bool /*isWriteable*/)
    {
        return p_->data_;
    }

    int MemIo::munmap()
    {
        return 0;
    }

    long MemIo::tell() const
    {
        return static_cast<long>(p_->idx_);
    }

    size_t MemIo::size() const
    {
        return p_->size_;
    }

    int MemIo::open()
    {
        p_->idx_ = 0;
        p_->eof_ = false;
        return 0;
    }

    bool MemIo::isopen() const
    {
        return true;
    }

    int MemIo::close()
    {
        return 0;
    }

    DataBuf MemIo::read(size_t rcount)
    {
        DataBuf buf(rcount);
        size_t readCount = read(buf.data(), buf.size());
        buf.resize(readCount);
        return buf;
    }

    size_t MemIo::read(byte* buf, size_t rcount)
    {
        const size_t avail = std::max(p_->size_ - p_->idx_, static_cast<size_t>(0));
        const size_t allow = std::min(rcount, avail);
        if (allow > 0) {
            std::memcpy(buf, &p_->data_[p_->idx_], allow);
        }
        p_->idx_ += allow;
        if (rcount > avail) {
          p_->eof_ = true;
        }
        return allow;
    }

    int MemIo::getb()
    {
        if (p_->idx_ >= p_->size_) {
            p_->eof_ = true;
            return EOF;
        }
        return p_->data_[p_->idx_++];
    }

    int MemIo::error() const
    {
        return 0;
    }

    bool MemIo::eof() const
    {
        return p_->eof_;
    }

    const std::string& MemIo::path() const noexcept
    {
        static std::string _path{"MemIo"};
        return _path;
    }

    void MemIo::populateFakeData() {

    }

#if EXV_XPATH_MEMIO
    XPathIo::XPathIo(const std::string& path) {
        Protocol prot = fileProtocol(path);

        if (prot == pStdin)         ReadStdin();
        else if (prot == pDataUri)  ReadDataUri(path);
    }

    void XPathIo::ReadStdin() {
        if (isatty(fileno(stdin)))
            throw Error(kerInputDataReadFailed);

#ifdef _O_BINARY
        // convert stdin to binary
        if (_setmode(_fileno(stdin), _O_BINARY) == -1)
            throw Error(kerInputDataReadFailed);
#endif

        char readBuf[100*1024];
        std::streamsize readBufSize = 0;
        do {
            std::cin.read(readBuf, sizeof(readBuf));
            readBufSize = std::cin.gcount();
            if (readBufSize > 0) {
                write((byte*)readBuf, (long)readBufSize);
            }
        } while(readBufSize);
    }

    void XPathIo::ReadDataUri(const std::string& path) {
        size_t base64Pos = path.find("base64,");
        if (base64Pos == std::string::npos)
            throw Error(kerErrorMessage, "No base64 data");

        std::string data = path.substr(base64Pos+7);
        char* decodeData = new char[data.length()];
        long size = base64decode(data.c_str(), decodeData, data.length());
        if (size > 0)
            write((byte*)decodeData, size);
        else
            throw Error(kerErrorMessage, "Unable to decode base 64.");
        delete[] decodeData;
    }

#else
    const std::string XPathIo::TEMP_FILE_EXT = ".exiv2_temp";
    const std::string XPathIo::GEN_FILE_EXT  = ".exiv2";

    XPathIo::XPathIo(const std::string& orgPath) : FileIo(XPathIo::writeDataToFile(orgPath)), isTemp_(true)
    {
        tempFilePath_ = path();
    }

    XPathIo::~XPathIo() {
        if (isTemp_ && !fs::remove(tempFilePath_)) {
            // error when removing file
            // printf ("Warning: Unable to remove the temp file %s.\n", tempFilePath_.c_str());
        }
    }

    void XPathIo::transfer(BasicIo& src) {
        if (isTemp_) {
            // replace temp path to gent path.
            auto currentPath = path();
            ReplaceStringInPlace(currentPath, XPathIo::TEMP_FILE_EXT, XPathIo::GEN_FILE_EXT);
            setPath(currentPath);

            tempFilePath_ = path();
            fs::rename(currentPath, tempFilePath_);
            isTemp_ = false;
            // call super class method
            FileIo::transfer(src);
        }
    }

    std::string XPathIo::writeDataToFile(const std::string& orgPath) {
        Protocol prot = fileProtocol(orgPath);

        // generating the name for temp file.
        std::time_t timestamp = std::time(nullptr);
        std::stringstream ss;
        ss << timestamp << XPathIo::TEMP_FILE_EXT;
        std::string path = ss.str();

        if (prot == pStdin) {
            if (isatty(fileno(stdin)))
                throw Error(kerInputDataReadFailed);
#if defined(_MSC_VER) || defined(__MINGW__)
            // convert stdin to binary
            if (_setmode(_fileno(stdin), _O_BINARY) == -1)
                throw Error(kerInputDataReadFailed);
#endif
            std::ofstream fs(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            // read stdin and write to the temp file.
            char readBuf[100*1024];
            std::streamsize readBufSize = 0;
            do {
                std::cin.read(readBuf, sizeof(readBuf));
                readBufSize = std::cin.gcount();
                if (readBufSize > 0) {
                    fs.write (readBuf, readBufSize);
                }
            } while(readBufSize);
            fs.close();
        } else if (prot == pDataUri) {
            std::ofstream fs(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            // read data uri and write to the temp file.
            size_t base64Pos = orgPath.find("base64,");
            if (base64Pos == std::string::npos) {
                fs.close();
                throw Error(kerErrorMessage, "No base64 data");
            }

            std::string data = orgPath.substr(base64Pos+7);
            std::vector<char> decodeData (data.length());
            long size = base64decode(data.c_str(), decodeData.data(), data.length());
            if (size > 0) {
                fs.write(decodeData.data(), size);
                fs.close();
            } else {
                fs.close();
                throw Error(kerErrorMessage, "Unable to decode base 64.");
            }
        }

        return path;
    }


#endif

    //! Internal Pimpl abstract structure of class RemoteIo.
    class RemoteIo::Impl {
    public:
        //! Constructor
        Impl(const std::string& url, size_t blockSize);
        //! Destructor. Releases all managed memory.
        virtual ~Impl();

        // DATA
        std::string     path_;          //!< (Standard) path
        size_t          blockSize_;     //!< Size of the block memory.
        BlockMap*       blocksMap_;     //!< An array contains all blocksMap
        size_t          size_;          //!< The file size
        size_t          idx_;           //!< Index into the memory area
        bool            isMalloced_;    //!< Was the blocksMap_ allocated?
        bool            eof_;           //!< EOF indicator
        Protocol        protocol_;      //!< the protocol of url
        size_t          totalRead_;     //!< bytes requested from host

        // METHODS
        /*!
          @brief Get the length (in bytes) of the remote file.
          @return Return -1 if the size is unknown. Otherwise it returns the length of remote file (in bytes).
          @throw Error if the server returns the error code.
         */
        virtual long getFileLength() = 0;
        /*!
          @brief Get the data by range.
          @param lowBlock The start block index.
          @param highBlock The end block index.
          @param response The data from the server.
          @throw Error if the server returns the error code.
          @note Set lowBlock = -1 and highBlock = -1 to get the whole file content.
         */
        virtual void getDataByRange(long lowBlock, long highBlock, std::string& response) = 0;
        /*!
          @brief Submit the data to the remote machine. The data replace a part of the remote file.
                The replaced part of remote file is indicated by from and to parameters.
          @param data The data are submitted to the remote machine.
          @param size The size of data.
          @param from The start position in the remote file where the data replace.
          @param to The end position in the remote file where the data replace.
          @note The write access is available on some protocols. HTTP and HTTPS require the script file
                on the remote machine to handle the data. SSH requires the permission to edit the file.
          @throw Error if it fails.
         */
        virtual void writeRemote(const byte* data, size_t size, long from, long to) = 0;
        /*!
          @brief Get the data from the remote machine and write them to the memory blocks.
          @param lowBlock The start block index.
          @param highBlock The end block index.
          @return Number of bytes written to the memory block successfully
          @throw Error if it fails.
         */
        virtual size_t populateBlocks(size_t lowBlock, size_t highBlock);

    }; // class RemoteIo::Impl

    RemoteIo::Impl::Impl(const std::string& url, size_t blockSize)
        : path_(url),
          blockSize_(blockSize),
          blocksMap_(nullptr),
          size_(0),
          idx_(0),
          isMalloced_(false),
          eof_(false),
          protocol_(fileProtocol(url)),
          totalRead_(0)
    {
    }

    size_t RemoteIo::Impl::populateBlocks(size_t lowBlock, size_t highBlock)
    {
        assert(isMalloced_);

        // optimize: ignore all true blocks on left & right sides.
        while(!blocksMap_[lowBlock].isNone()  && lowBlock  < highBlock) lowBlock++;
        while(!blocksMap_[highBlock].isNone() && highBlock > lowBlock)  highBlock--;

        size_t rcount = 0;
        if (blocksMap_[highBlock].isNone())
        {
            std::string data;
            getDataByRange(static_cast<long>(lowBlock), static_cast<long>(highBlock), data);
            rcount = data.length();
            if (rcount == 0) {
                throw Error(kerErrorMessage, "Data By Range is empty. Please check the permission.");
            }
            auto source = reinterpret_cast<byte*>(const_cast<char*>(data.c_str()));
            size_t remain = rcount, totalRead = 0;
            size_t iBlock = (rcount == size_) ? 0 : lowBlock;

            while (remain) {
                size_t allow = std::min(remain, blockSize_);
                blocksMap_[iBlock].populate(&source[totalRead], allow);
                remain -= allow;
                totalRead += allow;
                iBlock++;
            }
        }

        return rcount;
    }

    RemoteIo::Impl::~Impl() {
        delete[] blocksMap_;
    }

    RemoteIo::RemoteIo() = default;

    RemoteIo::~RemoteIo()
    {
        if (p_) {
            close();
        }
    }

    int RemoteIo::open()
    {
        close(); // reset the IO position
        bigBlock_ = nullptr;
        if (!p_->isMalloced_) {
            long length = p_->getFileLength();
            if (length < 0) { // unable to get the length of remote file, get the whole file content.
                std::string data;
                p_->getDataByRange(-1, -1, data);
                p_->size_ = data.length();
                size_t nBlocks = (p_->size_ + p_->blockSize_ - 1) / p_->blockSize_;
                p_->blocksMap_  = new BlockMap[nBlocks];
                p_->isMalloced_ = true;
                auto source = reinterpret_cast<byte*>(const_cast<char*>(data.c_str()));
                size_t remain = p_->size_, iBlock = 0, totalRead = 0;
                while (remain) {
                    size_t allow = std::min(remain, p_->blockSize_);
                    p_->blocksMap_[iBlock].populate(&source[totalRead], allow);
                    remain -= allow;
                    totalRead += allow;
                    iBlock++;
                }
            } else if (length == 0) { // file is empty
                throw Error(kerErrorMessage, "the file length is 0");
            } else {
                p_->size_ = static_cast<size_t>(length);
                size_t nBlocks = (p_->size_ + p_->blockSize_ - 1) / p_->blockSize_;
                p_->blocksMap_  = new BlockMap[nBlocks];
                p_->isMalloced_ = true;
            }
        }
        return 0; // means OK
    }

    int RemoteIo::close()
    {
        if (p_->isMalloced_) {
            p_->eof_ = false;
            p_->idx_ = 0;
        }
#ifdef EXIV2_DEBUG_MESSAGES
        std::cerr << "RemoteIo::close totalRead_ = " << p_->totalRead_ << std::endl;
#endif
        if ( bigBlock_ ) {
            delete [] bigBlock_;
            bigBlock_ = nullptr;
        }
        return 0;
    }

    size_t RemoteIo::write(const byte* /* unused data*/, size_t /* unused wcount*/)
    {
        return 0; // means failure
    }

    size_t RemoteIo::write(BasicIo& src)
    {
        assert(p_->isMalloced_);
        if (!src.isopen()) return 0;

        /*
         * The idea is to compare the file content, find the different bytes and submit them to the remote machine.
         * To simplify it, it:
         *      + goes from the left, find the first different position -> $left
         *      + goes from the right, find the first different position -> $right
         * The different bytes are [$left-$right] part.
         */
        size_t left       = 0;
        size_t right      = 0;
        size_t blockIndex = 0;
        std::vector<byte> buf(p_->blockSize_);
        size_t nBlocks    = (p_->size_ + p_->blockSize_ - 1) / p_->blockSize_;

        // find $left
        src.seek(0, BasicIo::beg);
        bool findDiff = false;
        while (blockIndex < nBlocks && !src.eof() && !findDiff) {
            size_t blockSize = p_->blocksMap_[blockIndex].getSize();
            bool isFakeData = p_->blocksMap_[blockIndex].isKnown(); // fake data
            size_t readCount = src.read(buf.data(), blockSize);
            byte* blockData = p_->blocksMap_[blockIndex].getData();
            for (size_t i = 0; (i < readCount) && (i < blockSize) && !findDiff; i++) {
                if ((!isFakeData && buf[i] != blockData[i]) || (isFakeData && buf[i] != 0)) {
                    findDiff = true;
                } else {
                    left++;
                }
            }
            blockIndex++;
        }

        // find $right
        findDiff    = false;
        blockIndex  = nBlocks;
        while (blockIndex > 0 && right < src.size() && !findDiff) {
            blockIndex--;
            size_t blockSize = p_->blocksMap_[blockIndex].getSize();
            if(src.seek(-1 * (blockSize + right), BasicIo::end)) {
                findDiff = true;
            } else {
                bool isFakeData = p_->blocksMap_[blockIndex].isKnown(); // fake data
                size_t readCount = src.read(buf.data(), blockSize);
                byte* blockData = p_->blocksMap_[blockIndex].getData();
                for (size_t i = 0; (i < readCount) && (i < blockSize) && !findDiff; i++) {
                    if ((!isFakeData && buf[readCount - i - 1] != blockData[blockSize - i - 1]) || (isFakeData && buf[readCount - i - 1] != 0)) {
                        findDiff = true;
                    } else {
                        right++;
                    }
                }
            }
        }

        // submit to the remote machine.
        auto dataSize = static_cast<long>(src.size() - left - right);
        if (dataSize > 0) {
            auto data = static_cast<byte*>(std::malloc(dataSize));
            src.seek(left, BasicIo::beg);
            src.read(data, dataSize);
            p_->writeRemote(data, dataSize, static_cast<long>(left), static_cast<long>(p_->size_ - right));
            if (data)
                std::free(data);
        }
        return static_cast<long>(src.size());
    }

    int RemoteIo::putb(byte /*unused data*/)
    {
        return 0;
    }

    DataBuf RemoteIo::read(size_t rcount)
    {
        DataBuf buf(rcount);
        size_t readCount = read(buf.data(), buf.size());
        if (readCount == 0) {
            throw Error(kerInputDataReadFailed);
        }
        buf.resize(readCount);
        return buf;
    }

    size_t RemoteIo::read(byte* buf, size_t rcount)
    {
        assert(p_->isMalloced_);
        if (p_->eof_)
            return 0;
        p_->totalRead_ += rcount;

        size_t allow     = std::min(rcount, ( p_->size_ - p_->idx_));
        size_t lowBlock  =  p_->idx_         /p_->blockSize_;
        size_t highBlock = (p_->idx_ + allow)/p_->blockSize_;

        // connect to the remote machine & populate the blocks just in time.
        p_->populateBlocks(lowBlock, highBlock);
        auto fakeData = static_cast<byte*>(std::calloc(p_->blockSize_, sizeof(byte)));
        if (!fakeData) {
            throw Error(kerErrorMessage, "Unable to allocate data");
        }

        size_t iBlock = lowBlock;
        size_t startPos = p_->idx_ - lowBlock*p_->blockSize_;
        size_t totalRead = 0;
        do {
            byte* data = p_->blocksMap_[iBlock++].getData();
            if (data == nullptr)
                data = fakeData;
            size_t blockR = std::min(allow, p_->blockSize_ - startPos);
            std::memcpy(&buf[totalRead], &data[startPos], blockR);
            totalRead += blockR;
            startPos = 0;
            allow -= blockR;
        } while(allow);

        std::free(fakeData);

        p_->idx_ += totalRead;
        p_->eof_ = (p_->idx_ == p_->size_);

        return totalRead;
    }

    int RemoteIo::getb()
    {
        assert(p_->isMalloced_);
        if (p_->idx_ == p_->size_) {
            p_->eof_ = true;
            return EOF;
        }

        size_t expectedBlock = p_->idx_/p_->blockSize_;
        // connect to the remote machine & populate the blocks just in time.
        p_->populateBlocks(expectedBlock, expectedBlock);

        byte* data = p_->blocksMap_[expectedBlock].getData();
        return data[p_->idx_++ - expectedBlock*p_->blockSize_];
    }

    void RemoteIo::transfer(BasicIo& src)
    {
        if (src.open() != 0) {
            throw Error(kerErrorMessage, "unable to open src when transferring");
        }
        write(src);
        src.close();
    }

    int RemoteIo::seek( int64_t offset, Position pos)
    {
        assert(p_->isMalloced_);
        int64_t newIdx = 0;

        switch (pos) {
            case BasicIo::cur: newIdx = p_->idx_ + offset; break;
            case BasicIo::beg: newIdx = offset; break;
            case BasicIo::end: newIdx = p_->size_ + offset; break;
        }

        // #1198.  Don't return 1 when asked to seek past EOF.  Stay calm and set eof_
        // if (newIdx < 0 || newIdx > (long) p_->size_) return 1;
        p_->idx_ = static_cast<size_t>(newIdx);
        p_->eof_ = newIdx > static_cast<int64_t>(p_->size_);
        if (p_->idx_ > p_->size_)
            p_->idx_ = p_->size_;
        return 0;
    }

    byte* RemoteIo::mmap(bool /*isWriteable*/)
    {
        size_t nRealData = 0 ;
        if ( !bigBlock_ ) {
            size_t blockSize = p_->blockSize_;
            size_t blocks = (p_->size_ + blockSize -1)/blockSize ;
            bigBlock_   = new byte[blocks*blockSize] ;
            for ( size_t block = 0 ; block < blocks ; block ++ ) {
                void* p = p_->blocksMap_[block].getData();
                if  ( p ) {
                    size_t nRead = block==(blocks-1)?p_->size_-nRealData:blockSize;
                    memcpy(bigBlock_+(block*blockSize),p,nRead);
                    nRealData   += nRead ;
                }
            }
#ifdef EXIV2_DEBUG_MESSAGES
            std::cerr << "RemoteIo::mmap nRealData = " << nRealData << std::endl;
#endif
        }

        return bigBlock_;
    }

    int RemoteIo::munmap()
    {
        return 0;
    }

    long RemoteIo::tell() const
    {
        return static_cast<long>(p_->idx_);
    }

    size_t RemoteIo::size() const
    {
        return static_cast<long>(p_->size_);
    }

    bool RemoteIo::isopen() const
    {
        return p_->isMalloced_;
    }

    int RemoteIo::error() const
    {
        return 0;
    }

    bool RemoteIo::eof() const
    {
        return p_->eof_;
    }

    const std::string& RemoteIo::path() const noexcept
    {
        return p_->path_;
    }

    void RemoteIo::populateFakeData()
    {
        assert(p_->isMalloced_);
        size_t nBlocks = (p_->size_ + p_->blockSize_ - 1) / p_->blockSize_;
        for (size_t i = 0; i < nBlocks; i++) {
            if (p_->blocksMap_[i].isNone())
                p_->blocksMap_[i].markKnown(p_->blockSize_);
        }
    }


    //! Internal Pimpl structure of class HttpIo.
    class HttpIo::HttpImpl : public Impl  {
    public:
        //! Constructor
        HttpImpl(const std::string& url, size_t blockSize);
        Exiv2::Uri hostInfo_; //!< the host information extracted from the path

        // METHODS
        /*!
          @brief Get the length (in bytes) of the remote file.
          @return Return -1 if the size is unknown. Otherwise it returns the length of remote file (in bytes).
          @throw Error if the server returns the error code.
         */
        long getFileLength() override;
        /*!
          @brief Get the data by range.
          @param lowBlock The start block index.
          @param highBlock The end block index.
          @param response The data from the server.
          @throw Error if the server returns the error code.
          @note Set lowBlock = -1 and highBlock = -1 to get the whole file content.
         */
        void getDataByRange(long lowBlock, long highBlock, std::string& response) override;
        /*!
          @brief Submit the data to the remote machine. The data replace a part of the remote file.
                The replaced part of remote file is indicated by from and to parameters.
          @param data The data are submitted to the remote machine.
          @param size The size of data.
          @param from The start position in the remote file where the data replace.
          @param to The end position in the remote file where the data replace.
          @note The data are submitted to the remote machine via POST. This requires the script file
                on the remote machine to receive the data and edit the remote file. The server-side
                script may be specified with the environment string EXIV2_HTTP_POST. The default value is
                "/exiv2.php". More info is available at http://dev.exiv2.org/wiki/exiv2
          @throw Error if it fails.
         */
        void writeRemote(const byte* data, size_t size, long from, long to) override;

        // NOT IMPLEMENTED
        HttpImpl(const HttpImpl& rhs) = delete;             //!< Copy constructor
        HttpImpl& operator=(const HttpImpl& rhs) = delete;  //!< Assignment
    }; // class HttpIo::HttpImpl

    HttpIo::HttpImpl::HttpImpl(const std::string& url, size_t blockSize):Impl(url, blockSize)
    {
        hostInfo_ = Exiv2::Uri::Parse(url);
        Exiv2::Uri::Decode(hostInfo_);
    }

    long HttpIo::HttpImpl::getFileLength()
    {
        Exiv2::Dictionary response;
        Exiv2::Dictionary request;
        std::string errors;
        request["server"] = hostInfo_.Host;
        request["page"  ] = hostInfo_.Path;
        if (!hostInfo_.Port.empty())
            request["port"] = hostInfo_.Port;
        request["verb"]   = "HEAD";
        int serverCode = http(request, response, errors);
        if (serverCode < 0 || serverCode >= 400 || !errors.empty()) {
            throw Error(kerFileOpenFailed, "http",Exiv2::Internal::stringFormat("%d",serverCode), hostInfo_.Path);
        }

        auto lengthIter = response.find("Content-Length");
        return (lengthIter == response.end()) ? -1 : atol((lengthIter->second).c_str());
    }

    void HttpIo::HttpImpl::getDataByRange(long lowBlock, long highBlock, std::string& response)
    {
        Exiv2::Dictionary responseDic;
        Exiv2::Dictionary request;
        request["server"] = hostInfo_.Host;
        request["page"  ] = hostInfo_.Path;
        if (!hostInfo_.Port.empty())
            request["port"] = hostInfo_.Port;
        request["verb"]   = "GET";
        std::string errors;
        if (lowBlock > -1 && highBlock > -1) {
            std::stringstream ss;
            ss << "Range: bytes=" << lowBlock * blockSize_  << "-" << ((highBlock + 1) * blockSize_ - 1) << "\r\n";
            request["header"] = ss.str();
        }

        int serverCode = http(request, responseDic, errors);
        if (serverCode < 0 || serverCode >= 400 || !errors.empty()) {
            throw Error(kerFileOpenFailed, "http",Exiv2::Internal::stringFormat("%d",serverCode), hostInfo_.Path);
        }
        response = responseDic["body"];
    }

    void HttpIo::HttpImpl::writeRemote(const byte* data, size_t size, long from, long to)
    {
        std::string scriptPath(getEnv(envHTTPPOST));
        if (scriptPath.empty()) {
            throw Error(kerErrorMessage, "Please set the path of the server script to handle http post data to EXIV2_HTTP_POST environmental variable.");
        }

        // standardize the path without "/" at the beginning.
        std::size_t protocolIndex = scriptPath.find("://");
        if (protocolIndex == std::string::npos && scriptPath[0] != '/') {
            scriptPath = "/" + scriptPath;
        }

        Exiv2::Dictionary response;
        Exiv2::Dictionary request;
        std::string errors;

        Uri scriptUri = Exiv2::Uri::Parse(scriptPath);
        request["server"] = scriptUri.Host.empty() ? hostInfo_.Host : scriptUri.Host;
        if (!scriptUri.Port.empty())
            request["port"] = scriptUri.Port;
        request["page"] = scriptUri.Path;
        request["verb"] = "POST";

        // encode base64
        size_t encodeLength = ((size + 2) / 3) * 4 + 1;
        std::vector<char> encodeData (encodeLength);
        base64encode(data, size, encodeData.data(), encodeLength);
        // url encode
        const std::string urlencodeData = urlencode(encodeData.data());

        std::stringstream ss;
        ss << "path="   << hostInfo_.Path << "&"
           << "from="   << from           << "&"
           << "to="     << to             << "&"
           << "data="   << urlencodeData;
        std::string postData = ss.str();

        // create the header
        ss.str("");
        ss << "Content-Length: " << postData.length()  << "\n"
           << "Content-Type: application/x-www-form-urlencoded\n"
           << "\n" << postData << "\r\n";
        request["header"] = ss.str();

        int serverCode = http(request, response, errors);
        if (serverCode < 0 || serverCode >= 400 || !errors.empty()) {
            throw Error(kerFileOpenFailed, "http",Exiv2::Internal::stringFormat("%d",serverCode), hostInfo_.Path);
        }
    }

    HttpIo::HttpIo(const std::string& url, size_t blockSize)
    {
        p_ = std::make_unique<HttpImpl>(url, blockSize);
    }

#ifdef EXV_USE_CURL
    //! Internal Pimpl structure of class RemoteIo.
    class CurlIo::CurlImpl : public Impl  {
    public:
        //! Constructor
        CurlImpl(const std::string&  path, size_t blockSize);
        //! Destructor. Cleans up the curl pointer and releases all managed memory.
        ~CurlImpl() override;

        CURL*        curl_;             //!< libcurl pointer

        // METHODS
        /*!
          @brief Get the length (in bytes) of the remote file.
          @return Return -1 if the size is unknown. Otherwise it returns the length of remote file (in bytes).
          @throw Error if the server returns the error code.
         */
        long getFileLength() override;
        /*!
          @brief Get the data by range.
          @param lowBlock The start block index.
          @param highBlock The end block index.
          @param response The data from the server.
          @throw Error if the server returns the error code.
          @note Set lowBlock = -1 and highBlock = -1 to get the whole file content.
         */
        void getDataByRange(long lowBlock, long highBlock, std::string& response) override;
        /*!
          @brief Submit the data to the remote machine. The data replace a part of the remote file.
                The replaced part of remote file is indicated by from and to parameters.
          @param data The data are submitted to the remote machine.
          @param size The size of data.
          @param from The start position in the remote file where the data replace.
          @param to The end position in the remote file where the data replace.
          @throw Error if it fails.
          @note The write access is only available on HTTP & HTTPS protocols. The data are submitted to server
                via POST method. It requires the script file on the remote machine to receive the data
                and edit the remote file. The server-side script may be specified with the environment
                string EXIV2_HTTP_POST. The default value is "/exiv2.php". More info is available at
                http://dev.exiv2.org/wiki/exiv2
         */
        void writeRemote(const byte* data, size_t size, long from, long to) override;

        // NOT IMPLEMENTED
        CurlImpl(const CurlImpl& rhs) = delete;             //!< Copy constructor
        CurlImpl& operator=(const CurlImpl& rhs) = delete;  //!< Assignment
    private:
        long timeout_; //!< The number of seconds to wait while trying to connect.
    }; // class RemoteIo::Impl

    CurlIo::CurlImpl::CurlImpl(const std::string& url, size_t blockSize) : Impl(url, blockSize), curl_(curl_easy_init())
    {
        if(!curl_) {
            throw Error(kerErrorMessage, "Unable to init libcurl.");
        }

        // The default block size for FTP is much larger than other protocols
        // the reason is that getDataByRange() in FTP always creates the new connection,
        // so we need the large block size to reduce the overhead of creating the connection.
        if (blockSize_ == 0) {
            blockSize_ = protocol_ == pFtp ? 102400 : 1024;
        }

        std::string timeout = getEnv(envTIMEOUT);
        timeout_ = atol(timeout.c_str());
        if (timeout_ == 0) {
            throw Error(kerErrorMessage, "Timeout Environmental Variable must be a positive integer.");
        }
    }

    long CurlIo::CurlImpl::getFileLength()
    {
        curl_easy_reset(curl_); // reset all options
        std::string response;
        curl_easy_setopt(curl_, CURLOPT_URL, path_.c_str());
        curl_easy_setopt(curl_, CURLOPT_NOBODY, 1); // HEAD
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curlWriter);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, timeout_);
        //curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1); // debugging mode

        /* Perform the request, res will get the return code */
        CURLcode res = curl_easy_perform(curl_);
        if(res != CURLE_OK) { // error happened
            throw Error(kerErrorMessage, curl_easy_strerror(res));
        }
        // get status
        int serverCode;
        curl_easy_getinfo (curl_, CURLINFO_RESPONSE_CODE, &serverCode); // get code
        if (serverCode >= 400 || serverCode < 0) {
            throw Error(kerFileOpenFailed, "http",Exiv2::Internal::stringFormat("%d",serverCode),path_);
        }
        // get length
        double temp;
        curl_easy_getinfo(curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &temp); // return -1 if unknown
        return static_cast<long>(temp);
    }

    void CurlIo::CurlImpl::getDataByRange(long lowBlock, long highBlock, std::string& response)
    {
        curl_easy_reset(curl_); // reset all options
        curl_easy_setopt(curl_, CURLOPT_URL, path_.c_str());
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L); // no progress meter please
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, curlWriter);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, timeout_);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);

        //curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1); // debugging mode

        if (lowBlock > -1 && highBlock> -1) {
            std::stringstream ss;
            ss << lowBlock * blockSize_  << "-" << ((highBlock + 1) * blockSize_ - 1);
            std::string range = ss.str();
            curl_easy_setopt(curl_, CURLOPT_RANGE, range.c_str());
        }

        /* Perform the request, res will get the return code */
        CURLcode res = curl_easy_perform(curl_);

        if(res != CURLE_OK) {
            throw Error(kerErrorMessage, curl_easy_strerror(res));
        }
        int serverCode;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &serverCode);  // get code
        if (serverCode >= 400 || serverCode < 0) {
            throw Error(kerFileOpenFailed, "http", Exiv2::Internal::stringFormat("%d", serverCode), path_);
        }
    }

    void CurlIo::CurlImpl::writeRemote(const byte* data, size_t size, long from, long to)
    {
        std::string scriptPath(getEnv(envHTTPPOST));
        if (scriptPath.empty()) {
            throw Error(kerErrorMessage, "Please set the path of the server script to handle http post data to EXIV2_HTTP_POST environmental variable.");
        }

        Exiv2::Uri hostInfo = Exiv2::Uri::Parse(path_);

        // add the protocol and host to the path
        std::size_t protocolIndex = scriptPath.find("://");
        if (protocolIndex == std::string::npos) {
            if (scriptPath[0] != '/') scriptPath = "/" + scriptPath;
            scriptPath = hostInfo.Protocol + "://" + hostInfo.Host + scriptPath;
        }

        curl_easy_reset(curl_); // reset all options
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L); // no progress meter please
        //curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1); // debugging mode
        curl_easy_setopt(curl_, CURLOPT_URL, scriptPath.c_str());
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);


        // encode base64
        size_t encodeLength = ((size + 2) / 3) * 4 + 1;
        std::vector<char> encodeData (encodeLength);
        base64encode(data, size, encodeData.data(), encodeLength);
        // url encode
        const std::string urlencodeData = urlencode(encodeData.data());
        std::stringstream ss;
        ss << "path="       << hostInfo.Path << "&"
           << "from="       << from          << "&"
           << "to="         << to            << "&"
           << "data="       << urlencodeData;
        std::string postData = ss.str();

        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, postData.c_str());
        // Perform the request, res will get the return code.
        CURLcode res = curl_easy_perform(curl_);

        if(res != CURLE_OK) {
            throw Error(kerErrorMessage, curl_easy_strerror(res));
        }
        int serverCode;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &serverCode);
        if (serverCode >= 400 || serverCode < 0) {
            throw Error(kerFileOpenFailed, "http", Exiv2::Internal::stringFormat("%d", serverCode), path_);
        }
    }

    CurlIo::CurlImpl::~CurlImpl() {
        curl_easy_cleanup(curl_);
    }

    size_t CurlIo::write(const byte* data, size_t wcount)
    {
        if (p_->protocol_ == pHttp || p_->protocol_ == pHttps) {
            return RemoteIo::write(data, wcount);
        }
        throw Error(kerErrorMessage, "doesnt support write for this protocol.");
    }

    size_t CurlIo::write(BasicIo& src)
    {
        if (p_->protocol_ == pHttp || p_->protocol_ == pHttps) {
            return RemoteIo::write(src);
        }
        throw Error(kerErrorMessage, "doesnt support write for this protocol.");
    }

    CurlIo::CurlIo(const std::string& url, size_t blockSize)
    {
        p_ = std::make_unique<CurlImpl>(url, blockSize);
    }

#endif

    // *************************************************************************
    // free functions

    DataBuf readFile(const std::string& path)
    {
        FileIo file(path);
        if (file.open("rb") != 0) {
            throw Error(kerFileOpenFailed, path, "rb", strError());
        }
        struct stat st;
        if (0 != ::stat(path.c_str(), &st)) {
            throw Error(kerCallFailed, path, strError(), "::stat");
        }
        DataBuf buf(st.st_size);
        const size_t len = file.read(buf.data(), buf.size());
        if (len != buf.size()) {
            throw Error(kerCallFailed, path, strError(), "FileIo::read");
        }
        return buf;
    }

    size_t writeFile(const DataBuf& buf, const std::string& path)
    {
        FileIo file(path);
        if (file.open("wb") != 0) {
            throw Error(kerFileOpenFailed, path, "wb", strError());
        }
        return file.write(buf.c_data(), buf.size());
    }


#ifdef EXV_USE_CURL
    size_t curlWriter(char* data, size_t size, size_t nmemb,
                      std::string* writerData)
    {
        if (writerData == NULL) return 0;
        writerData->append(data, size*nmemb);
        return size * nmemb;
    }
#endif
}                                       // namespace Exiv2
