/*
 * Minimal C++ runtime support for the bare-metal Playdate target.
 * The Playdate link_map.ld pulls in libgcc/libc/libm but not libstdc++.
 * Providing these operators/stubs avoids undefined-symbol link errors.
 */

#include <stdlib.h>

/* new / delete use the C heap (malloc/free from newlib) */
void *operator new  (size_t n)            { return malloc(n);  }
void *operator new[](size_t n)            { return malloc(n);  }
void  operator delete  (void *p) noexcept { free(p);           }
void  operator delete[](void *p) noexcept { free(p);           }
void  operator delete  (void *p, size_t)  noexcept { free(p);  }
void  operator delete[](void *p, size_t)  noexcept { free(p);  }

/* Called when a pure-virtual function is invoked — should never happen */
extern "C" void __cxa_pure_virtual(void) { for(;;); }

/* Called at static initialiser time to register destructors — stub it out */
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }
