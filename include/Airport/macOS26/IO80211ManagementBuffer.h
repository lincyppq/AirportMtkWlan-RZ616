/*
 * Tahoe 25G241 management-buffer calls used by the AWDL object.
 * Adapted from ForbiddenItlwm 69e88fa include/Airport/macOS26/IO80211ManagementBuffer.h.
 * bpfTapInput transfers the buffer on every normal return. Do not return it again.
 * GPLv2.
 */
#pragma once
#include <libkern/c++/OSObject.h>
class IO80211Buffer : public OSObject {
    IO80211Buffer();
public:
    virtual void returnBuffer();
    virtual void zero();
    virtual bool isZero() const;
    virtual size_t getLength() const;
    virtual void setLength(size_t);
    virtual size_t getCapacity() const;
private:
    virtual void _cursor() __asm__("__ZN13IO80211Buffer9getCursorEv");
    virtual void _constCursor() __asm__("__ZNK13IO80211Buffer9getCursorEv");
public:
    virtual void *getBytesNoCopy();
};
class IO80211BufferPool : public OSObject {
    IO80211BufferPool();
public:
    virtual IO80211Buffer *getBuffer();
};
class IO80211Controller;
class IO80211VirtualInterface;
extern "C" IO80211BufferPool *itlGetMgmtFramePool(IO80211Controller *)
    __asm__("__ZN17IO80211Controller16getMgmtFramePoolEv");
extern "C" void itlSubmitMgmtFrame(IO80211VirtualInterface *, IO80211Buffer *, unsigned int, void *, unsigned long)
    __asm__("__ZN23IO80211VirtualInterface11bpfTapInputEP13IO80211BufferjPvm");
