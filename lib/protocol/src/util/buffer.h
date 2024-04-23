#pragma once

#include <spark_wiring_vector.h>
#include <spark_wiring_error.h>

namespace particle::util {

class Buffer {
public:
    Buffer() = default;

    explicit Buffer(size_t size) :
            d_(size) {
    }

    Buffer(const char* data, size_t size) :
            d_(data, size) {
    }

    char* data() {
        return d_.data();
    }

    const char* data() const {
        return d_.data();
    }

    size_t size() const {
        return d_.size();
    }

    int resize(size_t size) {
        if (!d_.resize(size)) {
            return Error::NO_MEMORY;
        }
        return 0;
    }

private:
    Vector<char> d_;
};

} // namespace particle::util
