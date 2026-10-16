#pragma once

#include "../include/command_admission.hpp"

namespace exchange::core::admission {

// Isolated component fixtures have no lifecycle controller. Production opening remains private.
class CommandAdmissionIndexTestAccess final {
public:
    static void setAdmissionOpen(CommandAdmissionIndex& index, const bool open) {
        index.setAdmissionOpen(open);
    }

    static bool admissionOpen(const CommandAdmissionIndex& index) {
        std::lock_guard lock(index.mutex_);
        return index.admissionOpen_;
    }
};

} // namespace exchange::core::admission
