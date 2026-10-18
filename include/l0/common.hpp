#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace l0 {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
// Зарезервированное значение не выдаётся документам и обозначает исчерпание итератора.
constexpr uint32_t END = UINT32_MAX;
constexpr uint32_t BLOCK = 128;
/// @brief Проверяет условие; при нарушении выбрасывает std::runtime_error с текстом why.
inline void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::runtime_error(why);
}
/// @brief Дописывает 32-битное беззнаковое число в буфер в порядке little-endian.
inline void put32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (8 * i)));
}
/// @brief Дописывает 64-битное беззнаковое число в буфер в порядке little-endian.
inline void put64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(uint8_t(v >> (8 * i)));
}
/// @brief Читает число little-endian из четырёх доступных байтов по адресу p.
/// Вызывающий код обязан проверить границы; выравнивание адреса не требуется.
inline uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
/// @brief Читает число little-endian из восьми доступных байтов по адресу p.
/// Вызывающий код обязан проверить границы; выравнивание адреса не требуется.
inline uint64_t get64(const uint8_t* p) {
    return get32(p) | uint64_t(get32(p + 4)) << 32;
}
/// @brief Дописывает double IEEE 754 в little-endian, сохраняя точное значение score.
inline void put_double(Bytes& b, double value) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put64(b, bits);
}
/// @brief Восстанавливает double IEEE 754 из восьми доступных байтов little-endian.
inline double get_double(const uint8_t* p) {
    uint64_t bits = get64(p);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
/// @brief Записывает весь буфер в поток и сообщает исключением об ошибке записи.
inline void write(std::ostream& out, const Bytes& b) {
    out.write(reinterpret_cast<const char*>(b.data()), b.size());
    require(bool(out), "write failed (disk full?)");
}
/// @brief Возвращает текущую позицию записи; при ошибке потока выбрасывает исключение.
inline uint64_t tell(std::ostream& out) {
    auto p = out.tellp();
    require(p >= 0, "tellp failed");
    return uint64_t(p);
}
/// @brief Дописывает длину строки u32 и её байты; отклоняет строки длиннее UINT32_MAX.
inline void string_bytes(Bytes& b, std::string_view s) {
    require(s.size() <= UINT32_MAX, "string too large");
    put32(b, uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
struct Cursor {
    const uint8_t* p;
    size_t left;
    /// @brief Выдаёт следующие n байтов и продвигает курсор; проверяет остаток данных.
    /// Возвращённый указатель действителен, пока существует исходный буфер.
    const uint8_t* take(size_t n) {
        require(n <= left, "truncated/corrupt index");
        auto q = p;
        p += n;
        left -= n;
        return q;
    }
    /// @brief Читает u32 little-endian и сдвигает курсор на четыре байта.
    uint32_t u32() { return get32(take(4)); }
    /// @brief Читает u64 little-endian и сдвигает курсор на восемь байтов.
    uint64_t u64() { return get64(take(8)); }
    /// @brief Читает строку с префиксом длины u32 и возвращает независимую копию.
    std::string str() {
        auto n = u32();
        auto q = take(n);
        return std::string(reinterpret_cast<const char*>(q), n);
    }
};
class MappedFile {
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

  public:
    /// @brief Открывает файл и отображает его в память только для чтения.
    /// Пустой файл допустим; ошибки открытия и mmap приводят к исключению.
    explicit MappedFile(const fs::path& path);
    /// @brief Освобождает отображение файла, если оно было создано.
    ~MappedFile();
    /// @brief Запрещает копирование владельца отображения во избежание двойного munmap.
    MappedFile(const MappedFile&) = delete;
    /// @brief Запрещает копирующее присваивание владельца отображения.
    MappedFile& operator=(const MappedFile&) = delete;
    /// @brief Возвращает размер отображённого файла в байтах.
    size_t size() const { return size_; }
    /// @brief Возвращает адрес диапазона [off, off + len), проверяя границы без переполнения.
    /// Указатель действителен до уничтожения MappedFile; для пустого файла равен nullptr.
    const uint8_t* at(uint64_t off, uint64_t len) const {
        // Проверка через разность исключает переполнение суммы off + len.
        require(off <= size_ && len <= size_ - off, "index offset out of bounds");
        return data_ ? data_ + off : nullptr;
    }
    /// @brief Создаёт курсор чтения от смещения off до конца отображённого файла.
    Cursor cursor(uint64_t off = 0) const {
        require(off <= size_, "index offset out of bounds");
        return {at(off, size_ - off), size_ - size_t(off)};
    }
};
} // namespace l0
