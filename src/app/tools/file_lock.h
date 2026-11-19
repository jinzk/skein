#ifndef SKEIN_FILE_LOCK_H
#define SKEIN_FILE_LOCK_H

#include <string>

enum class lock_mode { shared, exclusive };

/* Cross-process file lock.  The lock is released automatically when the
 * process exits, even on abnormal termination. */
class file_lock {
public:
    file_lock() = default;
    ~file_lock();

    file_lock(const file_lock &) = delete;
    file_lock &operator=(const file_lock &) = delete;
    file_lock(file_lock &&) = delete;
    file_lock &operator=(file_lock &&) = delete;

    bool acquire(const std::string &file, const std::string &label,
                 lock_mode mode, std::string &error);

private:
    void release();

    bool locked_ = false;
#ifdef _WIN32
    void *handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

bool acquire_module_lock(const std::string &module, const std::string &version,
                         file_lock &lock, lock_mode mode, std::string &error);
bool acquire_project_lock(const std::string &root, file_lock &lock,
                          lock_mode mode, std::string &error);
bool acquire_cache_lock(file_lock &lock, lock_mode mode, std::string &error);

#endif
