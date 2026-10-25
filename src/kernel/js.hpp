#ifndef KERNEL_JS_HPP
#define KERNEL_JS_HPP

#include "types.hpp"

// AetherScript: a compact, self-contained JavaScript interpreter for the
// AetherOS web browser. It implements a practical subset of ECMAScript
// (expressions, control flow including switch, functions/closures, arrow
// functions, arrays, objects, JSON, and a small standard library) sufficient
// to run simple page scripts.
//
// It is intentionally lenient: property access on undefined/null and calls
// to non-functions yield undefined instead of throwing, so that scripts on
// real pages degrade gracefully rather than aborting the render.

namespace js {

// Host callbacks let the browser hook console output and document.write.
struct Host {
    void* user = nullptr;
    void (*log)(void* user, const char* message) = nullptr;        // console.log / alert
    void (*write)(void* user, const char* html) = nullptr;         // document.write
    // Fetch a URL and return the response body (synchronously).
    // Returns a heap-allocated string (caller frees) or nullptr on failure.
    char* (*fetch)(void* user, const char* url) = nullptr;         // fetch(url)
};

struct Result {
    bool ok = true;            // false if the script was empty/unparseable
    int statements = 0;        // number of top-level statements executed
};

// Execute a script. Returns true if the script was parsed and run.
bool execute(const char* source, Host* host = nullptr);

} // namespace js

#endif // KERNEL_JS_HPP
