/*
 * MtkAwdlRawUserClient.cpp — root-only IOUserClient for the M3a AWDL raw channel.
 * GPLv2.
 */
#include "MtkAwdlRawUserClient.hpp"
#include "AirportItlwmV2.hpp"
#include <HAL/MtkAwdlRawApi.h>
#ifdef MTK_AWDL_CAP_PROBE
#include "MtkAwdlCap.hpp"
#endif
#include <IOKit/IOLib.h>
#include <string.h>
#include <stddef.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(MtkAwdlRawUserClient, IOUserClient)

bool
MtkAwdlRawUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
    fTask = owningTask;
    fProvider = NULL;
    if (type != MTK_AWDLRAW_USERCLIENT_TYPE)
        return false;
    if (IOUserClient::clientHasPrivilege(securityID,
            kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
        IOLog("AirportMtkWlan: awdlraw deny non-root\n");
        return false;
    }
    if (!mwx_awdlraw_api_present()) {
        IOLog("AirportMtkWlan: awdlraw not present (need -mtkawdltx)\n");
        return false;
    }
    return super::initWithTask(owningTask, securityID, type, properties);
}

bool
MtkAwdlRawUserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    fProvider = OSDynamicCast(AirportItlwm, provider);
    return fProvider != NULL;
}

void
MtkAwdlRawUserClient::stop(IOService *provider)
{
    fProvider = NULL;
    super::stop(provider);
}

IOReturn
MtkAwdlRawUserClient::clientClose(void)
{
    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

IOReturn
MtkAwdlRawUserClient::clientDied(void)
{
    return super::clientDied();
}

IOReturn
MtkAwdlRawUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch; (void)target; (void)reference;
    if (!mwx_awdlraw_api_present())
        return kIOReturnNotOpen;

    switch (selector) {
    case kMtkAwdlRawGetInfo: {
        struct mtk_awdlraw_info info;
        if (args->structureOutput == NULL ||
            args->structureOutputSize < sizeof(info))
            return kIOReturnBadArgument;
        memset(&info, 0, sizeof(info));
        mwx_awdlraw_api_info(&info);
        memcpy(args->structureOutput, &info, sizeof(info));
        args->structureOutputSize = sizeof(info);
        return kIOReturnSuccess;
    }
    case kMtkAwdlRawRxRead: {
        if (args->structureOutput == NULL || args->structureOutputSize < 16)
            return kIOReturnBadArgument;
        uint32_t n = mwx_awdlraw_api_rx_read(args->structureOutput,
            args->structureOutputSize);
        args->structureOutputSize = n;
        return kIOReturnSuccess;
    }
    case kMtkAwdlRawTx: {
        const struct mtk_awdlraw_tx *req;
        struct mtk_awdlraw_txack ack;
        if (args->structureInput == NULL ||
            args->structureInputSize < offsetof(struct mtk_awdlraw_tx, frame) ||
            args->structureOutput == NULL ||
            args->structureOutputSize < sizeof(ack))
            return kIOReturnBadArgument;
        req = (const struct mtk_awdlraw_tx *)args->structureInput;
        if (req->len > MTK_AWDLRAW_MAX_FRAME ||
            args->structureInputSize <
            offsetof(struct mtk_awdlraw_tx, frame) + req->len)
            return kIOReturnBadArgument;
        memset(&ack, 0, sizeof(ack));
        int rv = mwx_awdlraw_api_tx(req, &ack);
        memcpy(args->structureOutput, &ack, sizeof(ack));
        args->structureOutputSize = sizeof(ack);
        return rv == 0 ? kIOReturnSuccess : kIOReturnError;
    }
    case kMtkAwdlRawTxsRead: {
        if (args->structureOutput == NULL || args->structureOutputSize < 16)
            return kIOReturnBadArgument;
        uint32_t n = mwx_awdlraw_api_txs_read(args->structureOutput,
            args->structureOutputSize);
        args->structureOutputSize = n;
        return kIOReturnSuccess;
    }
#ifdef MTK_AWDL_CAP_PROBE
    case kMtkAwdlCapGet:
    case kMtkAwdlCapSet: {
        /* Validate everything before any state changes. */
        const struct mtk_awdlcap_set *req = NULL;
        if (args->structureOutput == NULL ||
            args->structureOutputSize < sizeof(struct mtk_awdlcap_state))
            return kIOReturnBadArgument;
        if (selector == kMtkAwdlCapSet) {
            if (args->structureInput == NULL ||
                args->structureInputSize < sizeof(*req))
                return kIOReturnBadArgument;
            req = (const struct mtk_awdlcap_set *)args->structureInput;
            if (req->enabled > 1)
                return kIOReturnBadArgument;
            if (!mtk_awdlcap_set(req->enabled))
                return kIOReturnNotReady;
        }
        /* Return the state (after the change, for Set). */
        if (!mtk_awdlcap_read((struct mtk_awdlcap_state *)args->structureOutput))
            return kIOReturnNotReady;
        args->structureOutputSize = sizeof(struct mtk_awdlcap_state);
        return kIOReturnSuccess;
    }
#endif
    default:
        return kIOReturnUnsupported;
    }
}
