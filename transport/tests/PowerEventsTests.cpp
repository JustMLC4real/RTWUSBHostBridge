#include "../PowerEvents.hpp"
#include <cassert>
#include <cstdio>

int main() {
    rtw::PowerEvents p;
    assert(p.didChange(1)==0);p.start();
    assert(p.didChange(2)==0 && p.didChange(2)==0); // Initial on is not resume.
    assert(p.didChange(1)==p.LegacySuspend && p.didChange(1)==0);
    assert(p.didChange(2)==p.LegacyResume && p.didChange(2)==0);
    assert(p.didChange(99)==0); // Unknown state cannot break edge detection.
    assert(p.didChange(1)==p.LegacySuspend);
    assert(p.didChange(0)==0 && p.didChange(2)==0); // Off means termination, not resume.
    assert(p.didChange(1)==p.LegacySuspend);p.stop();
    assert(p.didChange(2)==0 && p.didChange(1)==0);
    p.start();assert(p.didChange(1)==p.LegacySuspend);
    assert(p.didChange(2)==p.LegacyResume);p.stop();
    std::puts("PASS: native power edges, duplicate suppression, initial on, off, unknown state, teardown");
}
