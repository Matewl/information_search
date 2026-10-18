#include "l0/codec.hpp"
namespace l0::p4 {
/// @brief Дописывает беззнаковое число в формате LEB128: семь бит данных на байт.
static void varint(Bytes& out, uint32_t x) {
    while (x >= 128) {
        out.push_back(uint8_t(x) | 128);
        x >>= 7;
    }
    out.push_back(uint8_t(x));
}
/// @brief Читает u32 LEB128 из курсора; отклоняет переполнение и оборванную последовательность.
static uint32_t unvarint(Cursor& c) {
    uint32_t x = 0;
    // Для u32 пятый байт может содержать только четыре младших бита.
    for (unsigned s = 0; s <= 28; s += 7) {
        uint8_t b = *c.take(1);
        require(s != 28 || b < 16, "invalid varint");
        x |= uint32_t(b & 127) << s;
        if (!(b & 128))
            return x;
    }
    throw std::runtime_error("invalid varint");
}
/// @brief Сжимает до 128 целых, выбирая ширину упакованных битов по минимальному размеру.
/// Старшие биты исключений записываются отдельно; исходный массив не изменяется.
void encode(const uint32_t* a, size_t n, Bytes& out) {
    require(n > 0 && n <= BLOCK, "invalid PForDelta block length");
    unsigned best = 32;
    size_t cost = n * 4;
    // Гистограмма разрядностей позволяет оценивать исключения для всех b
    // без повторного обхода исходных значений при каждой пробной ширине.
    size_t widths[33] = {};
    for (size_t i = 0; i < n; ++i)
        ++widths[a[i] ? 32 - __builtin_clz(a[i]) : 0];
    for (unsigned b = 0; b < 32; ++b) {
        size_t c = (n * b + 7) / 8;
        for (unsigned w = b + 1; w <= 32; ++w)
            // Исключение: один байт индекса и ceil((w - b) / 7) байтов старших битов.
            c += widths[w] * (1 + (w - b + 6) / 7);
        if (c < cost) {
            cost = c;
            best = b;
        }
    }
    uint8_t exceptions = 0;
    if (best < 32)
        for (size_t i = 0; i < n; ++i)
            if (a[i] >> best)
                ++exceptions;
    out.push_back(uint8_t(n));
    out.push_back(uint8_t(best));
    out.push_back(exceptions);
    size_t start = out.size(), bytes = (n * best + 7) / 8;
    out.resize(start + bytes, 0);
    // 64-битный промежуточный тип нужен и для b=32, и для сдвига внутри байта.
    uint64_t mask = best == 32 ? UINT32_MAX : ((uint64_t(1) << best) - 1);
    for (size_t i = 0; i < n && best; ++i) {
        size_t bit = i * best;
        uint64_t v = (uint64_t(a[i]) & mask) << (bit % 8);
        size_t k = (best + bit % 8 + 7) / 8;
        for (size_t j = 0; j < k; ++j)
            out[start + bit / 8 + j] |= uint8_t(v >> (8 * j));
    }
    // Позиции исключений идут по возрастанию: декодер проверяет это свойство.
    if (best < 32)
        for (size_t i = 0; i < n; ++i)
            if (a[i] >> best) {
                out.push_back(uint8_t(i));
                varint(out, a[i] >> best);
            }
}
/// @brief Восстанавливает значения фрейма из младших битов и исключений.
/// Проверяет заголовок, порядок исключений и границы чисел, продвигая входной курсор.
std::vector<uint32_t> decode(Cursor& c) {
    auto h = c.take(3);
    size_t n = h[0];
    unsigned b = h[1], ne = h[2];
    require(n > 0 && n <= BLOCK && b <= 32 && ne <= n && (b != 32 || !ne),
            "invalid PForDelta header");
    auto p = c.take((n * b + 7) / 8);
    std::vector<uint32_t> out(n, 0);
    uint64_t mask = b == 32 ? UINT32_MAX : ((uint64_t(1) << b) - 1);
    for (size_t i = 0; i < n && b; ++i) {
        size_t bit = i * b, k = (b + bit % 8 + 7) / 8;
        uint64_t v = 0;
        for (size_t j = 0; j < k; ++j)
            v |= uint64_t(p[bit / 8 + j]) << (8 * j);
        out[i] = uint32_t((v >> (bit % 8)) & mask);
    }
    // Сначала восстановлены младшие биты; исключения дополняют их старшими.
    int last = -1;
    for (unsigned i = 0; i < ne; ++i) {
        unsigned ix = *c.take(1);
        uint32_t hi = unvarint(c);
        require((ix < n) && (int(ix) > last) && hi && ((uint64_t(hi) << b) <= UINT32_MAX),
                "invalid PForDelta exception");
        out[ix] |= hi << b;
        last = int(ix);
    }
    return out;
}
/// @brief Дописывает последовательность независимых PForDelta-фреймов, включая неполный последний.
void encode_stream(const std::vector<uint32_t>& v, Bytes& out) {
    for (size_t i = 0; i < v.size(); i += BLOCK)
        encode(v.data() + i, std::min<size_t>(BLOCK, v.size() - i), out);
}
/// @brief Декодирует поток полностью и требует ровно expected восстановленных значений.
std::vector<uint32_t> decode_stream(Cursor c, size_t expected) {
    std::vector<uint32_t> r;
    // Грубая верхняя граница защищает reserve от недостоверного числа элементов.
    require(expected <= c.left * BLOCK, "invalid decoded size");
    r.reserve(expected);
    while (c.left) {
        auto v = decode(c);
        require(v.size() <= expected - r.size(), "PForDelta count mismatch");
        r.insert(r.end(), v.begin(), v.end());
    }
    require(r.size() == expected, "PForDelta count mismatch");
    return r;
}
} // namespace l0::p4
