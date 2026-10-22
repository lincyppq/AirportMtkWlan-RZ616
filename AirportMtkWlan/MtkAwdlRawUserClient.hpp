/*
 * MtkAwdlRawUserClient.hpp — root-only IOUserClient for the M3a AWDL raw channel.
 * Only registered when MTK_AWDL_TX is defined and -mtkawdltx is set. GPLv2.
 */
#ifndef MtkAwdlRawUserClient_hpp
#define MtkAwdlRawUserClient_hpp

#include <IOKit/IOUserClient.h>
#include <ClientKit/AwdlRaw.h>

class AirportItlwm;

class MtkAwdlRawUserClient : public IOUserClient {
    OSDeclareDefaultStructors(MtkAwdlRawUserClient)
public:
    virtual bool initWithTask(task_t owningTask, void *securityID,
        UInt32 type, OSDictionary *properties) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOReturn clientClose(void) override;
    virtual IOReturn clientDied(void) override;
    virtual IOReturn externalMethod(uint32_t selector,
        IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch = 0,
        OSObject *target = 0, void *reference = 0) override;
private:
    AirportItlwm *fProvider;
    task_t        fTask;
};
#endif
