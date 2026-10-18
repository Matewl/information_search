#pragma once
#include "index.hpp"
#include <functional>

namespace l0 {
/// Внешняя стабильная сортировка документов до присваивания docID и построения posting lists.
class RankedDocuments {
    struct Record {
        double score;
        uint64_t ordinal;
        uint32_t raw_size;
        Bytes payload;
    };
    fs::path scratch_;
    uint64_t budget_, memory_ = 0, count_ = 0, next_run_ = 0;
    size_t fan_in_;
    bool finished_ = false;
    std::vector<Record> pending_;
    std::vector<fs::path> runs_;
    /// @brief Сравнивает записи по убыванию score, затем по исходному порядку поступления.
    static bool before(const Record& a, const Record& b);
    /// @brief Возвращает уникальный путь очередной временной отсортированной порции.
    fs::path next_path();
    /// @brief Сохраняет заголовок и сжатую запись документа в поток.
    static void write_record(std::ostream& out, const Record& record);
    /// @brief Читает одну запись; возвращает false только при штатном конце файла.
    static bool read_record(std::istream& in, Record& record);
    /// @brief Сортирует накопленную порцию и записывает её на диск, освобождая память.
    void flush();
    /// @brief Сливает до fan_in_ порций; удаляет только собственные исчерпанные временные файлы.
    void merge(const std::vector<fs::path>& paths, const std::function<void(const Record&)>& emit);
    /// @brief Распаковывает документ и восстанавливает его исходные метаданные и score.
    static Document unpack(const Record& record);

  public:
    /// @brief Создаёт новый scratch-каталог, порог памяти и ограничение числа потоков слияния.
    /// Порог приблизительный; сверх него допускается текущий документ. fan_in должен быть >= 2.
    RankedDocuments(const fs::path& scratch, uint64_t memory_bytes, size_t fan_in = 32);
    /// @brief Удаляет собственные временные данные также при исключении во время сборки.
    ~RankedDocuments();
    /// @brief Запрещает копирование владельца временного каталога.
    RankedDocuments(const RankedDocuments&) = delete;
    /// @brief Запрещает копирующее присваивание владельца временного каталога.
    RankedDocuments& operator=(const RankedDocuments&) = delete;
    /// @brief Сжимает и добавляет документ в сортировку; score должен быть конечным числом.
    void add(const Document& document);
    /// @brief Передаёт все документы в пустой builder в порядке score по убыванию.
    /// При равных score сохраняет исходный порядок; builder.finish() вызывается отдельно.
    void index_into(Builder& builder);
};
} // namespace l0
