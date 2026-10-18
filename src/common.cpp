#include "l0/common.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace l0 {
/// @brief Создаёт read-only mmap файла; закрывает файловый дескриптор и при ошибке отображения.
MappedFile::MappedFile(const fs::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    require(fd >= 0, "cannot open " + path.string());
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        throw std::runtime_error("cannot stat index");
    }
    size_ = size_t(st.st_size);
    // Нулевую длину нельзя передать mmap: пустой файл представляем без отображения.
    if (size_) {
        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("mmap failed");
        }
        data_ = static_cast<const uint8_t*>(p);
    }
    // Отображение продолжает жить после закрытия дескриптора; тысячи сегментов
    // не удерживают тысячи открытых файлов одновременно.
    close(fd);
}
/// @brief Снимает отображение, не изменяя содержимое исходного файла.
MappedFile::~MappedFile() {
    if (data_)
        munmap(const_cast<uint8_t*>(data_), size_);
}
} // namespace l0
