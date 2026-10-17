#include "LegacyABI.hpp"
#include <IOKit/IOMessage.h>

OSDefineMetaClassAndStructors(RTUSBPipe, OSObject)
OSDefineMetaClassAndStructors(RTUSBDevice, IOService)
OSDefineMetaClassAndStructors(RTUSBInterface, IOService)

static_assert(IOUSBHostDevice::kPowerStateOff==0 &&
    IOUSBHostDevice::kPowerStateSuspended==1 && IOUSBHostDevice::kPowerStateOn==2,
    "Power event translator requires the reviewed native USB power ordinals");
static_assert(rtw::PowerEvents::LegacyResume==static_cast<uint32_t>(kUSBLegacyMessagePortHasBeenResumed) &&
    rtw::PowerEvents::LegacySuspend==static_cast<uint32_t>(kUSBLegacyMessagePortHasBeenSuspended),
    "Power event translator must preserve Realtek's legacy message values");

uintptr_t rtwUnsupportedABI(const char* type, uint32_t slot) {
    IOLog("RTWUSBHostBridge: unimplemented ABI slot %s + 0x%x; candidate is not qualified\n", type, slot);
    return static_cast<uint32_t>(kIOReturnUnsupported);
}

static bool redundantIdleTimeout(uint32_t idle, uint32_t total) {
    return !idle || (total && idle >= total);
}

static StandardUSB::DeviceRequest setup(const rtw::LegacyRequest& r) {
    StandardUSB::DeviceRequest q = {};
    q.bmRequestType = r.type; q.bRequest = r.request;
    q.wValue = r.value; q.wIndex = r.index; q.wLength = r.length;
    return q;
}

RTUSBPipe* RTUSBPipe::create(IOUSBHostInterface* interface, rtw::HostSession* session, IOUSBHostPipe* pipe) {
    if (!interface || !session) return nullptr;
    auto* result = new RTUSBPipe;
    if (!result) return nullptr;
    if (!result->init()) { result->release(); return nullptr; }
    result->host = interface; interface->retain();
    result->session = session; session->retain();
    result->endpoint = pipe;
    if (pipe) pipe->retain();
    return result;
}
void RTUSBPipe::free() {
    if (endpoint) endpoint->release();
    if (host) host->release();
    if (session) session->release();
    OSObject::free();
}
uint8_t RTUSBPipe::status() { return 0; } // Legacy GetStatus is specified to return 0.
IOReturn RTUSBPipe::pipeStatus() { return kIOReturnUnsupported; }
IOReturn RTUSBPipe::abort() {
    return session ? session->perform([](void* arg) -> IOReturn {
        auto* p=static_cast<RTUSBPipe*>(arg);
        return rtw::legacyStatus(p->endpoint ? p->endpoint->abort() : p->host->abortDeviceRequests());
    },this) : kIOReturnNotOpen;
}
IOReturn RTUSBPipe::reset() { return clearPipeStall(false); }
IOReturn RTUSBPipe::clearStall() { return clearPipeStall(false); }
IOReturn RTUSBPipe::clearPipeStall(bool request) {
    struct Arguments { RTUSBPipe* pipe; bool request; } args={this,request};
    return session ? session->perform([](void* arg) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        return a.pipe->endpoint ? rtw::legacyStatus(a.pipe->endpoint->clearStall(a.request)) : kIOReturnUnsupported;
    },&args) : kIOReturnNotOpen;
}
const StandardUSB::EndpointDescriptor* RTUSBPipe::endpointDescriptor() {
    return endpoint ? endpoint->getEndpointDescriptor() : nullptr;
}
uint8_t RTUSBPipe::direction() {
    const auto* d = endpointDescriptor(); return d ? (d->bEndpointAddress >> 7) : 0;
}
uint8_t RTUSBPipe::type() {
    const auto* d = endpointDescriptor(); return d ? (d->bmAttributes & 3) : 0;
}
uint8_t RTUSBPipe::endpointNumber() {
    const auto* d = endpointDescriptor(); return d ? (d->bEndpointAddress & 15) : 0;
}
uint16_t RTUSBPipe::address() {
    auto* device=host?host->getDevice():nullptr;
    return device?device->getAddress():0;
}
uint16_t RTUSBPipe::maxPacketSize() {
    const auto* d = endpointDescriptor(); return d ? (d->wMaxPacketSize & 0x7ff) : 0;
}
uint8_t RTUSBPipe::interval() {
    const auto* d = endpointDescriptor(); return d ? d->bInterval : 0;
}

IOReturn RTUSBPipe::transfer(IOMemoryDescriptor* buffer, uint32_t idle,
    uint32_t timeout, uint64_t length, rtw::LegacyCompletion* completion,
    uint64_t* done) {
    if (done) *done = 0;
    struct Arguments { RTUSBPipe* pipe; IOMemoryDescriptor* buffer; uint32_t idle,timeout;
        uint64_t length; rtw::LegacyCompletion* completion; uint64_t* done;
    } args={this,buffer,idle,timeout,length,completion,done};
    return session ? session->perform([](void* arg) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        return a.pipe->transferGated(a.buffer,a.idle,a.timeout,a.length,a.completion,a.done);
    },&args) : kIOReturnNotOpen;
}
IOReturn RTUSBPipe::transferGated(IOMemoryDescriptor* buffer, uint32_t idle,
    uint32_t timeout, uint64_t length, rtw::LegacyCompletion* completion,
    uint64_t* done) {
    if (!endpoint || !buffer) return kIOReturnBadArgument;
    if (!length) length = buffer->getLength();
    if (length > buffer->getLength() || length > 0xffffffffULL) return kIOReturnBadArgument;
    if (completion && completion->action) {
        // All audited Realtek submissions use the RtWlanU OSObject as target.
        // Retain the OSObject until callback. Its internal packet-queue lifetime
        // still requires independent teardown qualification.
        auto* owner = static_cast<OSObject*>(completion->target);
        if (!owner) return kIOReturnBadArgument;
        const auto lease=session->completionLease();
        return rtw::pipeAsync(endpoint, owner, buffer, length, *completion, idle, timeout, this, &lease);
    }
    if (idle) return kIOReturnUnsupported;
    if (type() == 3 && timeout) return kIOReturnBadArgument;
    if (type() != 2 && type() != 3) return kIOReturnUnsupported;
    uint32_t transferred = 0;
    const auto result = endpoint->io(buffer, static_cast<uint32_t>(length), transferred, timeout);
    if (done) *done = transferred > length ? length : transferred;
    return transferred > length ? kIOReturnOverrun : rtw::legacyStatus(result);
}
IOReturn RTUSBPipe::read(IOMemoryDescriptor* b, uint32_t idle, uint32_t timeout,
    uint64_t n, rtw::LegacyCompletion* c, uint64_t* done) {
    if (!direction()) return kIOReturnBadArgument;
    return transfer(b, idle, timeout, n, c, done);
}
IOReturn RTUSBPipe::write(IOMemoryDescriptor* b, uint32_t idle, uint32_t timeout,
    uint64_t n, rtw::LegacyCompletion* c) {
    if (direction()) return kIOReturnBadArgument;
    return transfer(b, idle, timeout, n, c, nullptr);
}
IOReturn RTUSBPipe::readTimed(IOMemoryDescriptor* b, uint32_t i, uint32_t t,
    rtw::LegacyCompletion* c, uint64_t* d) { return read(b,i,t,0,c,d); }
IOReturn RTUSBPipe::writeTimed(IOMemoryDescriptor* b, uint32_t i, uint32_t t,
    rtw::LegacyCompletion* c) { return write(b,i,t,0,c); }
IOReturn RTUSBPipe::readDefault(IOMemoryDescriptor* b, rtw::LegacyCompletion* c,
    uint64_t* d) { return read(b,0,0,0,c,d); }
IOReturn RTUSBPipe::writeDefault(IOMemoryDescriptor* b, rtw::LegacyCompletion* c) {
    return write(b,0,0,0,c);
}
IOReturn RTUSBPipe::control(rtw::LegacyRequest* r, uint32_t idle,
    uint32_t timeout, rtw::LegacyCompletion* c) {
    struct Arguments { RTUSBPipe* pipe; rtw::LegacyRequest* request;
        uint32_t idle,timeout; rtw::LegacyCompletion* completion;
    } args={this,r,idle,timeout,c};
    return session ? session->perform([](void* arg) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        return a.pipe->controlGated(a.request,a.idle,a.timeout,a.completion,false);
    },&args) : kIOReturnNotOpen;
}
IOReturn RTUSBPipe::controlGated(rtw::LegacyRequest* r, uint32_t idle,
    uint32_t timeout, rtw::LegacyCompletion* c, bool descriptor) {
    if (endpoint || !r) return kIOReturnBadArgument;
    if (!redundantIdleTimeout(idle, timeout)) return kIOReturnUnsupported;
    if (descriptor) {
        if (!r->data) return kIOReturnBadArgument;
        auto* buffer=static_cast<IOMemoryDescriptor*>(r->data);
        if (r->length>buffer->getLength()) return kIOReturnBadArgument;
        if (c && c->action) {
            const auto lease=session->completionLease();
            return rtw::controlAsync(host,
                static_cast<OSObject*>(c->target),*r,buffer,*c,timeout,this,&lease);
        }
        auto q=setup(*r);uint32_t done=0;r->bytesDone=0;
        const auto result=host->deviceRequest(q,buffer,done,timeout);
        r->bytesDone=done>r->length?r->length:done;
        return done>r->length?kIOReturnOverrun:rtw::legacyStatus(result);
    }
    if (c && c->action) {
        const auto lease=session->completionLease();
        return rtw::controlRawAsync(host,
            static_cast<OSObject*>(c->target), *r, *c, timeout, this,&lease);
    }
    return rtw::controlSync(host, *r, timeout);
}
IOReturn RTUSBPipe::controlDesc(rtw::LegacyRequest* r, uint32_t idle,
    uint32_t timeout, rtw::LegacyCompletion* c) {
    struct Arguments { RTUSBPipe* pipe; rtw::LegacyRequest* request;
        uint32_t idle,timeout; rtw::LegacyCompletion* completion;
    } args={this,r,idle,timeout,c};
    return session ? session->perform([](void* arg) -> IOReturn {
        auto& a=*static_cast<Arguments*>(arg);
        return a.pipe->controlGated(a.request,a.idle,a.timeout,a.completion,true);
    },&args) : kIOReturnNotOpen;
}
IOReturn RTUSBPipe::controlDefault(rtw::LegacyRequest* r, rtw::LegacyCompletion* c) {
    return control(r,0,5000,c);
}
IOReturn RTUSBPipe::controlDescDefault(rtw::LegacyRequest* r, rtw::LegacyCompletion* c) {
    return controlDesc(r,0,5000,c);
}

RTUSBDevice* RTUSBDevice::create(IOUSBHostInterface* interface) {
    if (!interface || !interface->getDevice()) return nullptr;
    auto* result = new RTUSBDevice;
    if (!result) return nullptr;
    if (!result->init()) { result->release(); return nullptr; }
    result->host = interface; interface->retain();
    return result;
}
bool RTUSBDevice::prepareSession(rtw::HostSession* session) {
    auto* replacement=RTUSBPipe::create(host,session);
    if (!replacement) return false;
    auto* old=zero;zero=replacement;
    if (old) old->release();
    return true;
}
void RTUSBDevice::free() {
    if (zero) zero->release();
    if (host) host->release();
    IOService::free();
}
const StandardUSB::DeviceDescriptor* RTUSBDevice::descriptor() {
    auto* device=host?host->getDevice():nullptr;
    return device?device->getDeviceDescriptor():nullptr;
}
const StandardUSB::ConfigurationDescriptor* RTUSBDevice::configuration(uint8_t index) {
    // The original ASUS path asks only for configuration index 0. Do not
    // accidentally return a different configuration for an unsupported index.
    return index == 0 ? host->getConfigurationDescriptor() : nullptr;
}
uint16_t RTUSBDevice::address() { auto* d=host?host->getDevice():nullptr;return d?d->getAddress():0; }
uint8_t RTUSBDevice::speed() { auto* d=host?host->getDevice():nullptr;return d?d->getSpeed():0; }
uint8_t RTUSBDevice::maxPacketSize() { const auto* d=descriptor(); return d?d->bMaxPacketSize0:0; }
uint16_t RTUSBDevice::vendor() { const auto* d=descriptor(); return d?d->idVendor:0; }
uint16_t RTUSBDevice::product() { const auto* d=descriptor(); return d?d->idProduct:0; }
uint16_t RTUSBDevice::revision() { const auto* d=descriptor(); return d?d->bcdDevice:0; }
uint8_t RTUSBDevice::configurationCount() { const auto* d=descriptor(); return d?d->bNumConfigurations:0; }
uint8_t RTUSBDevice::manufacturerIndex() { const auto* d=descriptor(); return d?d->iManufacturer:0; }
uint8_t RTUSBDevice::productIndex() { const auto* d=descriptor(); return d?d->iProduct:0; }
uint8_t RTUSBDevice::serialIndex() { const auto* d=descriptor(); return d?d->iSerialNumber:0; }
RTUSBPipe* RTUSBDevice::controlPipe() { return zero; }

IOReturn RTUSBDevice::findInterfaceDescriptor(
    const StandardUSB::ConfigurationDescriptor* configuration,
    const StandardUSB::InterfaceDescriptor* current,
    const InterfaceQuery* query, StandardUSB::InterfaceDescriptor** output) {
    if (!output || !query) return kIOReturnBadArgument;
    *output = nullptr;
    const auto* active = host->getConfigurationDescriptor();
    if (configuration && configuration != active) return kIOReturnUnsupported;
    if (!active) return kIOReturnNotReady;
    const auto* bytes = reinterpret_cast<const uint8_t*>(active);
    const uint32_t total = active->wTotalLength;
    uint32_t offset = active->bLength;
    if (offset < 9 || offset > total) return kIOReturnBadArgument;
    bool after = current == nullptr;
    bool currentFound = after;
    while (offset < total) {
        if (total-offset < 2 || bytes[offset]<2 || bytes[offset]>total-offset)
            return kIOReturnBadArgument;
        const auto* d = bytes+offset;
        if (d[1]==4) {
            if (d[0]<9) return kIOReturnBadArgument;
            auto* candidate=reinterpret_cast<const StandardUSB::InterfaceDescriptor*>(d);
            if (!after) {
                if (candidate==current) { after=true; currentFound=true; }
            } else if ((query->klass==0xffff || query->klass==d[5]) &&
                (query->subclass==0xffff || query->subclass==d[6]) &&
                (query->protocol==0xffff || query->protocol==d[7]) &&
                (query->alternate==0xffff || query->alternate==d[3])) {
                *output=const_cast<StandardUSB::InterfaceDescriptor*>(candidate);
                return kIOReturnSuccess;
            }
        }
        offset+=d[0];
    }
    return currentFound?kIOReturnNotFound:kIOReturnBadArgument;
}

IOReturn RTUSBDevice::stringDescriptor(uint8_t index,char* output,int capacity,uint16_t language) {
    if (!output || capacity<=0) return kIOReturnBadArgument;
    output[0]=0;
    if (!index) return kIOReturnBadArgument;
    const auto* d=reinterpret_cast<const uint8_t*>(host->getStringDescriptor(index,language));
    if (!d || d[0]<2 || (d[0]&1) || d[1]!=3) return kIOReturnNotFound;
    uint32_t written=0;
    for (uint32_t i=2;i<d[0];i+=2) {
        uint32_t cp=d[i]|(static_cast<uint32_t>(d[i+1])<<8);
        if (cp>=0xd800 && cp<=0xdbff) {
            if (i+3>=d[0]) return kIOReturnBadArgument;
            const uint32_t lo=d[i+2]|(static_cast<uint32_t>(d[i+3])<<8);
            if (lo<0xdc00 || lo>0xdfff) return kIOReturnBadArgument;
            cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);i+=2;
        } else if (cp>=0xdc00 && cp<=0xdfff) return kIOReturnBadArgument;
        uint8_t encoded[4];uint32_t n;
        if (cp<0x80) { encoded[0]=cp;n=1; }
        else if (cp<0x800) { encoded[0]=0xc0|(cp>>6);encoded[1]=0x80|(cp&63);n=2; }
        else if (cp<0x10000) { encoded[0]=0xe0|(cp>>12);encoded[1]=0x80|((cp>>6)&63);encoded[2]=0x80|(cp&63);n=3; }
        else { encoded[0]=0xf0|(cp>>18);encoded[1]=0x80|((cp>>12)&63);encoded[2]=0x80|((cp>>6)&63);encoded[3]=0x80|(cp&63);n=4; }
        if (written+n>=static_cast<uint32_t>(capacity)) return kIOReturnNoSpace;
        for (uint32_t j=0;j<n;++j) output[written++]=encoded[j];
        output[written]=0;
    }
    return kIOReturnSuccess;
}

bool RTUSBInterface::start(IOService* provider) {
    if (!IOService::start(provider)) return false;
    if (getProperty("RTWDevelopmentEnable") != kOSBooleanTrue) {
        IOLog("RTWUSBHostBridge: development build disabled; hardware qualification required\n");
        return false;
    }
    host = OSDynamicCast(IOUSBHostInterface, provider);
    if (!host) return false;
    host->retain();
    auto* device = host->getDevice();
    const auto* d = device?device->getDeviceDescriptor():nullptr;
    if (!d || d->idVendor != 0x0b05 || d->idProduct != 0x1817) return false;
    proxy = RTUSBDevice::create(host);
    if (!proxy) return false;
    updateProperties();
    // The bridge is an extra IOService node between the native interface and
    // the network controller. Join the parent power tree so it does not sever
    // the network driver's existing sleep/wake path. The native host continues
    // to own physical USB power management.
    PMinit();powerActive=true;
    static IOPMPowerState states[2] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,kIOPMDeviceUsable|kIOPMPowerOn,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}
    };
    if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) return false;
    host->joinPMtree(this);
    changePowerStateTo(1);
    // Native USB interfaces do not relay the compatibility layer's legacy
    // suspend/resume messages. Observe the actual device's completed power
    // transitions explicitly; this registration retains the bridge until
    // stopPowerNotifications deregisters and drains in-flight callbacks.
    powerProvider=device;powerProvider->retain();
    powerEvents.start();
    powerProvider->registerInterestedDriver(this);
    registerService();
    return true;
}
void RTUSBInterface::stop(IOService* provider) {
    stopping=true;
    stopPowerNotifications();
    if (powerActive) { powerActive=false;PMstop(); }
    closeSession();
    closePipes(); client = nullptr;
    IOService::stop(provider);
}
void RTUSBInterface::free() {
    stopping=true;
    stopPowerNotifications();
    if (powerActive) { powerActive=false;PMstop(); }
    closeSession();
    closePipes();
    if (proxy) proxy->release();
    if (session) session->release();
    if (host) host->release();
    IOService::free();
}
bool RTUSBInterface::handleOpen(IOService* forClient, IOOptionBits options, void* arg) {
    if (closing || stopping || isInactive() || client || pendingCloseClient || !host ||
        !IOService::handleOpen(forClient,options,arg)) return false;
    if (session) { session->release();session=nullptr; }
    session=rtw::HostSession::create(host);
    if (!session || !session->open(this)) {
        IOService::handleClose(forClient,options);return false;
    }
    hostOpen = true;
    closePipes(); // Refresh wrappers only when beginning a new open session.
    if (!proxy->prepareSession(session) || createPipes() != kIOReturnSuccess) {
        closeSession();
        IOService::handleClose(forClient, options); return false;
    }
    client = forClient;
    return true;
}
void RTUSBInterface::close(IOService* forClient,IOOptionBits options) {
    if (!lockForArbitration()) return;
    if (!session || !IOService::handleIsOpen(forClient)) {
        IOService::close(forClient,options);unlockForArbitration();return;
    }
    // Intercept BEFORE IOService::close: its scheduleStop decision does not
    // recheck handleIsOpen after handleClose, so deferring only handleClose is
    // insufficient. Keep ownership recorded until the worker runs base close.
    pendingCloseClient=forClient;hostOpen=false;client=nullptr;
    auto* closingSession=session;closingSession->retain();retain();forClient->retain();
    // Native close may wait for callbacks that themselves call this facade.
    // Do not hold IOService arbitration across that native drain.
    unlockForArbitration();
    const auto result=closingSession->closeAndReleaseClient(this,forClient,
        reinterpret_cast<void*>(static_cast<uintptr_t>(options)),
        [](IOService* target,IOService* owner,void* value) {
            static_cast<RTUSBInterface*>(target)->finishClientClose(owner,
                static_cast<IOOptionBits>(reinterpret_cast<uintptr_t>(value)));
        });
    if (result!=kIOReturnSuccess)
        IOLog("RTWUSBHostBridge: failed to queue client close (0x%x); open record retained\n",result);
    closingSession->release();
    forClient->release();release();
}
void RTUSBInterface::handleClose(IOService* forClient,IOOptionBits options) {
    if (forClient==client) closeSession();
    IOService::handleClose(forClient,options);
}
void RTUSBInterface::finishClientClose(IOService* forClient,IOOptionBits options) {
    // The worker holds no USB gate. Match IOService open/close arbitration,
    // then reenter the PUBLIC close path so its scheduleStop logic still runs.
    if (!lockForArbitration()) {
        IOLog("RTWUSBHostBridge: deferred client-close arbitration failed; open record retained\n");
        return;
    }
    IOService::close(forClient,options);
    pendingCloseClient=nullptr;
    unlockForArbitration();
}
void RTUSBInterface::closeSession() {
    if (closing) return;
    closing=true;client=nullptr;hostOpen=false;
    // Native close owns the synchronous cancellation/drain of every endpoint
    // and of this interface's control requests (IOUSBHostInterface.h). Issuing
    // separate asynchronous aborts first needlessly runs completions while the
    // native session is still open. Keep wrappers alive throughout the drain.
    // closing also rejects an inline callback attempting to reopen the facade.
    if (session) session->close();
    closing=false;
}
bool RTUSBInterface::willTerminate(IOService* provider, IOOptionBits options) {
    // Apple requires closing the native session during provider termination;
    // otherwise a removed device can keep the port unavailable for rematching.
    stopping=true;
    powerEvents.stop();
    closeSession();
    return IOService::willTerminate(provider,options);
}
bool RTUSBInterface::didTerminate(IOService* provider,IOOptionBits options,bool* defer) {
    if (!session) return IOService::didTerminate(provider,options,defer);
    if (!defer) return false;
    // didTerminate is the documented asynchronous stop-deferral point. Keep
    // the caller's stack bool local to this invocation; only options is copied.
    // The base method schedules stop, so invoke it only after tracked callbacks
    // and any registered termination-message delivery have returned.
    *defer=true;
    const auto result=session->closeAndFinalize(this,provider,
        reinterpret_cast<void*>(static_cast<uintptr_t>(options)),
        [](IOService* target,IOService* source,void* value) {
            bool pending=false;
            static_cast<RTUSBInterface*>(target)->IOService::didTerminate(source,
                static_cast<IOOptionBits>(reinterpret_cast<uintptr_t>(value)),&pending);
        });
    if (result!=kIOReturnSuccess)
        IOLog("RTWUSBHostBridge: failed to queue deferred didTerminate (0x%x); stop remains deferred\n",result);
    return true;
}
IOReturn RTUSBInterface::message(uint32_t type, IOService* provider, void* argument) {
    if (type==kIOMessageServiceIsTerminated) {
        stopping=true;
        powerEvents.stop();
        hostOpen=false;client=nullptr;
        if (session) {
            // A message can reenter from a native close completion. Queue it
            // on that immutable session instead of letting closeSession's
            // recursive-close guard notify Realtek before the drain ends.
            // IOService termination's argument is an opaque options value;
            // no pointee is accessed or retained by this deferred relay.
            return session->closeAndNotify(this,provider,argument,
                [](IOService* target,IOService* source,void* value) {
                    target->messageClients(kIOMessageServiceIsTerminated,value);
                    target->IOService::message(kIOMessageServiceIsTerminated,source,value);
                });
        }
    }
    messageClients(type, argument);
    return IOService::message(type, provider, argument);
}
void RTUSBInterface::stopPowerNotifications() {
    powerEvents.stop();
    auto* provider=powerProvider;
    // deRegisterInterestedDriver synchronizes with outstanding power callbacks.
    // Keep the provider member valid until that synchronization is complete.
    if (provider) {
        provider->deRegisterInterestedDriver(this);
        powerProvider=nullptr;provider->release();
    }
}
IOReturn RTUSBInterface::powerStateWillChangeTo(IOPMPowerFlags capabilities,
    unsigned long state, IOService* provider) {
    if (provider==powerProvider) return IOPMAckImplied;
    return IOService::powerStateWillChangeTo(capabilities,state,provider);
}
IOReturn RTUSBInterface::powerStateDidChangeTo(IOPMPowerFlags capabilities,
    unsigned long state, IOService* provider) {
    if (provider!=powerProvider)
        return IOService::powerStateDidChangeTo(capabilities,state,provider);
    const auto event=powerEvents.didChange(state);
    if (event) messageClients(event,nullptr);
    return IOPMAckImplied;
}
void RTUSBInterface::closePipes() {
    for (uint32_t i=0; i<count; ++i) {
        if (endpoints[i]) endpoints[i]->release();
        endpoints[i] = nullptr;
    }
    count = 0;
}
IOReturn RTUSBInterface::createPipes() {
    if (!hostOpen) return kIOReturnNotOpen;
    if (count) return kIOReturnSuccess;
    rtw::Endpoint selected[32] = {}; uint32_t n=0;
    auto result = rtw::copyEndpoints(host,selected,32,n);
    if (result != kIOReturnSuccess) return result;
    for (uint32_t i=0;i<n;++i) {
        endpoints[i]=RTUSBPipe::create(host,session,selected[i].pipe);
        if (!endpoints[i]) { result=kIOReturnNoMemory; break; }
        ++count;
    }
    rtw::releaseEndpoints(selected,n);
    if (result != kIOReturnSuccess) closePipes();
    return result;
}
void RTUSBInterface::updateProperties() {
    const char* keys[] = {"idVendor","idProduct","bcdDevice","bInterfaceNumber",
        "bConfigurationValue","bInterfaceClass","bInterfaceSubClass",
        "bInterfaceProtocol","locationID","USB Product Name"};
    for (auto key:keys) if (auto* value=host->getProperty(key)) setProperty(key,value);
}
const StandardUSB::InterfaceDescriptor* RTUSBInterface::descriptor() { return host->getInterfaceDescriptor(); }
RTUSBPipe* RTUSBInterface::pipe(uint8_t index) { return index<count?endpoints[index]:nullptr; }
RTUSBDevice* RTUSBInterface::device() { return proxy; }
uint8_t RTUSBInterface::configurationValue() { const auto* d=host->getConfigurationDescriptor();return d?d->bConfigurationValue:0; }
uint8_t RTUSBInterface::interfaceNumber() { const auto* d=descriptor();return d?d->bInterfaceNumber:0; }
uint8_t RTUSBInterface::alternateSetting() { const auto* d=descriptor();return d?d->bAlternateSetting:0; }
uint8_t RTUSBInterface::endpointCount() { const auto* d=descriptor();return d?d->bNumEndpoints:0; }
uint8_t RTUSBInterface::interfaceClass() { const auto* d=descriptor();return d?d->bInterfaceClass:0; }
uint8_t RTUSBInterface::interfaceSubClass() { const auto* d=descriptor();return d?d->bInterfaceSubClass:0; }
uint8_t RTUSBInterface::interfaceProtocol() { const auto* d=descriptor();return d?d->bInterfaceProtocol:0; }
uint8_t RTUSBInterface::interfaceStringIndex() { const auto* d=descriptor();return d?d->iInterface:0; }
IOReturn RTUSBInterface::alternate(IOService*,uint16_t setting) {
    // Replacing live pipes while Realtek retains legacy pointers is not safe.
    return setting==alternateSetting()?kIOReturnSuccess:kIOReturnUnsupported;
}
void RTUSBInterface::unlinkPipes() {
    for (uint32_t i=0;i<count;++i) if (endpoints[i]) endpoints[i]->abort();
    if (hostOpen) host->abortDeviceRequests();
}
void RTUSBInterface::reopenPipes() { if (hostOpen && !count) createPipes(); }
RTUSBPipe* RTUSBInterface::findPipe(RTUSBPipe* current,EndpointQuery* q) {
    return findPipeRetained(current,q,false);
}
RTUSBPipe* RTUSBInterface::findPipeRetained(RTUSBPipe* current,EndpointQuery* q,bool retain) {
    if (!q) return nullptr;
    bool after=current==nullptr;
    for (uint32_t i=0;i<count;++i) {
        auto* p=endpoints[i];
        if (!after) { if (p==current) after=true;continue; }
        if ((q->type!=0xff && p->type()!=q->type) ||
            (q->direction!=0xff && p->direction()!=q->direction)) continue;
        q->type=p->type();q->direction=p->direction();
        q->maxPacketSize=p->maxPacketSize();q->interval=p->interval();
        if (retain) p->retain();
        return p;
    }
    return nullptr;
}
IOReturn RTUSBInterface::endpointProperties(uint8_t alt,uint8_t number,uint8_t dir,
    uint8_t* transferType,uint16_t* packet,uint8_t* polling) {
    if (!transferType || !packet || !polling) return kIOReturnBadArgument;
    if (alt!=alternateSetting()) return kIOReturnUnsupported;
    for (uint32_t i=0;i<count;++i) {
        auto* p=endpoints[i];
        if (p->endpointNumber()==number && p->direction()==dir) {
            *transferType=p->type();*packet=p->maxPacketSize();*polling=p->interval();
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNotFound;
}
IOReturn RTUSBInterface::control(rtw::LegacyRequest* r,rtw::LegacyCompletion* c) {
    if (!hostOpen || !proxy) return kIOReturnNotOpen;
    return proxy->controlPipe()->control(r,0,5000,c);
}
IOReturn RTUSBInterface::controlDesc(rtw::LegacyRequest* r,rtw::LegacyCompletion* c) {
    if (!hostOpen || !proxy) return kIOReturnNotOpen;
    return proxy->controlPipe()->controlDesc(r,0,5000,c);
}
