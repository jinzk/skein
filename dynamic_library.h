#ifndef SKEIN_DYNAMIC_LIBRARY_H
#define SKEIN_DYNAMIC_LIBRARY_H

#include <memory>
#include <string>

/*
 * Small platform-level wrapper around dlopen/LoadLibrary.  The library stays
 * loaded until the last shared_ptr is released, which keeps in-flight FFI
 * calls safe when the script closes the user-visible handle.
 */
class dynamic_library {
public:
    static std::shared_ptr<dynamic_library> open(const std::string &path,
                                                 std::string &error);

    ~dynamic_library();

    dynamic_library(const dynamic_library &) = delete;
    dynamic_library &operator=(const dynamic_library &) = delete;

    void *symbol(const char *name, std::string &error) const;
    bool valid() const;
    const std::string &path() const;

private:
    dynamic_library(void *handle, std::string path);

    void *handle_ = nullptr;
    std::string path_;
};

#endif
