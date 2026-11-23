#ifndef SKEIN_STARTUP_PROFILE_H
#define SKEIN_STARTUP_PROFILE_H

#include <chrono>
#include <string>

namespace skein {
namespace startup_profile {

/* Startup profiling is enabled by setting SKEIN_STARTUP_STATS=1.  Marks are
 * only meaningful on the main thread while the interpreter is starting; the
 * profiler is not thread safe. */
bool enabled();
void begin();
void mark(const char *name);
void finish();

/* Records the lifetime of a scope.  Spans may nest; finish() prints them with
 * indentation so a package import can be seen together with its nested work. */
class span_scope {
  public:
    explicit span_scope(const char *name);
    explicit span_scope(const std::string &name);
    ~span_scope();

    span_scope(const span_scope &) = delete;
    span_scope &operator=(const span_scope &) = delete;

  private:
    bool active_;
    std::string name_;
    std::chrono::steady_clock::time_point started_;
};

} // namespace startup_profile
} // namespace skein

#endif
