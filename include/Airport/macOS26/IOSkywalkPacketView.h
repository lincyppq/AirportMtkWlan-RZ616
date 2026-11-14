/*
 * Tahoe 25G241 packet calls for the software Ethernet bridge.
 * Slot order adapted from ForbiddenItlwm 69e88fa
 * include/Airport/macOS26/IOSkywalkPacketView.h. GPLv2.
 * A successful enqueuePackets completes and disposes the packet.
 */
#pragma once
#include <libkern/c++/OSObject.h>
#include <IOKit/IOReturn.h>
class IOSkywalkPacketBufferPool;
class IOSkywalkPacketBuffer;
class IOMemoryDescriptor;
class ItlSkywalkPacketView : public OSObject {
    ItlSkywalkPacketView();
    virtual void _initSlot();
public:
    virtual unsigned int getPacketBuffers(IOSkywalkPacketBuffer **, unsigned int) const;
    virtual unsigned int getPacketBufferCount() const;
    virtual IOMemoryDescriptor *getMemoryDescriptor() const;
    virtual IOReturn setDataLength(unsigned int);
    virtual unsigned int getDataLength() const;
private:
    virtual void _offset1();
    virtual void _offset2();
    virtual void _offset3();
    virtual void _offset4();
    virtual void _offset5();
    virtual void _offset6();
public:
    virtual uint64_t getDataVirtualAddress() const;
private:
    virtual void _address2();
public:
    virtual IOReturn prepareWithQueue(void *, unsigned int, unsigned int);
private:
    virtual void _prepareSlot();
public:
    virtual IOReturn completeWithQueue(void *, unsigned int, unsigned int);
};
extern "C" IOReturn itlRxEnqueue(void *, ItlSkywalkPacketView *const *, unsigned int, unsigned int)
    __asm__("__ZN26IOSkywalkRxCompletionQueue14enqueuePacketsEPKP15IOSkywalkPacketjj");
extern "C" IOReturn itlPacketSetHeadroom(ItlSkywalkPacketView *, unsigned char)
    __asm__("__ZN22IOSkywalkNetworkPacket11setHeadroomEh");
extern "C" IOReturn itlPacketSetLinkHeaderLength(ItlSkywalkPacketView *, unsigned char)
    __asm__("__ZN22IOSkywalkNetworkPacket19setLinkHeaderLengthEh");
class ItlSkywalkPoolView : public OSObject {
    ItlSkywalkPoolView();
    virtual void _initSlot1();
    virtual void _initSlot2();
public:
    virtual IOReturn allocatePacket(ItlSkywalkPacketView **, unsigned int);
    virtual IOReturn allocatePacket(unsigned int, ItlSkywalkPacketView **, unsigned int);
private:
    virtual void _allocateManySlot();
public:
    virtual void deallocatePacket(ItlSkywalkPacketView *);
};
