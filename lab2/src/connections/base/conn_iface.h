#pragma once

#include <cstddef>
#include <string>

class ConnIface {
public:
    virtual ~ConnIface() = default;

    virtual bool readExact(void *buf, std::size_t count, int timeoutMs) = 0;

    virtual bool writeExact(const void *buf, std::size_t count, int timeoutMs) = 0;

    virtual void close() = 0;

    virtual std::string lastError() const = 0;
};
