#pragma once

#include <ydb-cpp-sdk/util/system/defaults.h>
#include <ydb-cpp-sdk/util/generic/ptr.h>

#include <cstddef>

/*
 * This is really fast buffer for temporary data.
 * For small sizes it works almost fast as pure alloca()
 * (by using perthreaded list of free blocks),
 * for big sizes it works as fast as malloc()/operator new()/...
 * Over-aligned types are not supported.
 */
class TTempBuf {
public:
    static constexpr std::size_t TmpBufLen = 64 * 1024;

    /*
     * we do not want many friends for this class :)
     */
    class TImpl;

    TTempBuf();
    TTempBuf(std::size_t len);
    TTempBuf(const TTempBuf& b) noexcept;
    TTempBuf(TTempBuf&& b) noexcept;
    ~TTempBuf();

    TTempBuf& operator=(const TTempBuf& b) noexcept;
    TTempBuf& operator=(TTempBuf&& b) noexcept;

    Y_PURE_FUNCTION char* Data() noexcept;

    Y_PURE_FUNCTION const char* Data() const noexcept;

    Y_PURE_FUNCTION char* Current() noexcept;

    Y_PURE_FUNCTION const char* Current() const noexcept;

    Y_PURE_FUNCTION std::size_t Size() const noexcept;

    Y_PURE_FUNCTION std::size_t Filled() const noexcept;

    Y_PURE_FUNCTION std::size_t Left() const noexcept;

    void Reset() noexcept;
    void SetPos(std::size_t off);
    char* Proceed(std::size_t off);
    void Append(const void* data, std::size_t len);

    Y_PURE_FUNCTION bool IsNull() const noexcept;

private:
    TIntrusivePtr<TImpl> Impl_;
};

template <typename T>
class TTempArray: private TTempBuf {
private:
    static T* TypedPointer(char* pointer) noexcept {
        return reinterpret_cast<T*>(pointer);
    }
    static const T* TypedPointer(const char* pointer) noexcept {
        return reinterpret_cast<const T*>(pointer);
    }
    static constexpr std::size_t RawSize(const std::size_t size) noexcept {
        return size * sizeof(T);
    }
    static constexpr std::size_t TypedSize(const std::size_t size) noexcept {
        return size / sizeof(T);
    }

public:
    TTempArray() = default;

    TTempArray(std::size_t len)
        : TTempBuf(RawSize(len))
    {
    }

    T* Data() noexcept {
        return TypedPointer(TTempBuf::Data());
    }

    const T* Data() const noexcept {
        return TypedPointer(TTempBuf::Data());
    }

    T* Current() noexcept {
        return TypedPointer(TTempBuf::Current());
    }

    const T* Current() const noexcept {
        return TypedPointer(TTempBuf::Current());
    }

    std::size_t Size() const noexcept {
        return TypedSize(TTempBuf::Size());
    }
    std::size_t Filled() const noexcept {
        return TypedSize(TTempBuf::Filled());
    }

    T* Proceed(std::size_t off) {
        return reinterpret_cast<T*>(TTempBuf::Proceed(RawSize(off)));
    }
};
