#include <IOKit/IOService.h>
class RTWLayoutProbe: public IOService { public: virtual int marker(); }; int RTWLayoutProbe::marker(){return 0;}
class RTWPipeLayoutProbe: public OSObject { public: virtual int marker(); }; int RTWPipeLayoutProbe::marker(){return 0;}
