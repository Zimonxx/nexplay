#include "storage/ReplayStorage.h"

#include <stdexcept>

namespace nexplay::storage {

ReplayStorage::ReplayStorage()
    : root_(std::filesystem::temp_directory_path() / L"NexPlay" / L"ReplayBuffer") {
}

const std::filesystem::path& ReplayStorage::root() const noexcept {
    return root_;
}

void ReplayStorage::ensureExists() const {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        throw std::runtime_error("Nie mozna utworzyc katalogu bufora powtorek.");
    }
}

} // namespace nexplay::storage
