#pragma once

#include <semaphore.h>
#include <string>

class NamedSemaphore {
public:
    NamedSemaphore() = default;

    ~NamedSemaphore();

    NamedSemaphore(const NamedSemaphore &) = delete;

    NamedSemaphore &operator=(const NamedSemaphore &) = delete;

    NamedSemaphore(NamedSemaphore &&other) noexcept;

    NamedSemaphore &operator=(NamedSemaphore &&other) noexcept;

    static NamedSemaphore create(const std::string &name, unsigned int initialValue, bool owner, std::string *err);

    static NamedSemaphore open(const std::string &name, bool owner, std::string *err);

    bool isValid() const { return sem_ != SEM_FAILED && sem_ != nullptr; }

    const std::string &name() const { return name_; }

    bool waitMs(int timeoutMs, std::string *err);

    bool post(std::string *err);

    void close();

private:
    sem_t *sem_ = nullptr;
    std::string name_;
    bool owner_ = false;
};

std::string makeSemName(const std::string &prefix, int hostPid, int clientId, const std::string &suffix);
