#pragma once
#include "common.hpp"
#include <unordered_map>

namespace l0 {
struct Span {
    uint32_t begin, end;
    /// @brief Сравнивает интервалы сначала по началу, затем по концу.
    bool operator<(const Span& b) const {
        return begin < b.begin || (begin == b.begin && end < b.end);
    }
    /// @brief Проверяет совпадение обеих включённых границ интервалов.
    bool operator==(const Span& b) const { return begin == b.begin && end == b.end; }
};
struct SearchStats {
    uint64_t blocks_decoded = 0, position_blocks_decoded = 0, skip_jumps = 0;
};
class Iterator {
  public:
    /// @brief Обеспечивает корректное уничтожение производного итератора через базовый указатель.
    virtual ~Iterator() = default;
    /// @brief Возвращает текущий docID или END, если итератор исчерпан.
    virtual uint32_t doc() const = 0;
    /// @brief Находит docID не меньше target, не перемещаясь назад; при исчерпании возвращает END.
    virtual uint32_t advance(uint32_t target) = 0;
    /// @brief Продвигается к следующему документу; END остаётся устойчивым состоянием.
    uint32_t next() { return doc() == END ? END : advance(doc() + 1); }
    /// @brief Возвращает оценку числа совпадений для выбора порядка пересечения.
    virtual uint64_t cost() const = 0;
    /// @brief Возвращает интервалы текущего документа; булевы AND/AND NOT возвращают пустой список.
    /// Ссылка принадлежит итератору; её содержимое может измениться после продвижения.
    virtual const std::vector<Span>& spans() = 0;
};
using Iter = std::unique_ptr<Iterator>;
struct Document {
    std::string id, url, text, source;
    uint64_t row = 0;
    double score = 0;
};
struct DocumentInfo {
    std::string id, url, source;
    uint64_t row = 0;
    double score = 0;
};
struct Posting {
    uint32_t doc;
    std::vector<uint32_t> positions;
};

class Builder {
    fs::path root_;
    uint64_t budget_, memory_ = 0, token_count_ = 0;
    uint32_t docs_ = 0, segment_start_ = 0;
    std::unordered_map<std::string, std::vector<Posting>> terms_;
    std::ofstream docs_data_, docs_offsets_;
    struct SegmentInfo {
        std::string name;
        uint32_t first, count;
    };
    std::vector<SegmentInfo> segments_;
    bool done_ = false;
    double last_score_ = std::numeric_limits<double>::infinity();
    /// @brief Записывает накопленные документы в отдельный сегмент и освобождает рабочий словарь.
    void flush();

  public:
    /// @brief Создаёт новый каталог индекса и задаёт приблизительный порог выгрузки в байтах.
    /// Существующий каталог не перезаписывается; memory_bytes должен быть положительным.
    explicit Builder(const fs::path& root, uint64_t memory_bytes = 256ull * 1024 * 1024);
    /// @brief Индексирует следующий документ уже отсортированного по убыванию score потока.
    /// Сохраняет score, проверяет порядок и при необходимости выгружает сегмент.
    void add(const Document& doc);
    /// @brief Выгружает остаток, закрывает файлы и публикует манифест готового индекса.
    /// Вызывать ровно один раз после добавления всех документов.
    void finish();
    /// @brief Возвращает число уже добавленных документов, включая документы без токенов.
    uint32_t document_count() const { return docs_; }
    /// @brief Возвращает суммарное число проиндексированных вхождений термов.
    uint64_t token_count() const { return token_count_; }
};

class Segment {
  public:
    MappedFile terms, keys, trie, postings, skips;
    uint32_t first, count;
    /// @brief Отображает файлы сегмента с заданным префиксом и диапазоном docID.
    Segment(const fs::path& prefix, uint32_t first, uint32_t count);
    /// @brief Ищет нормализованный терм в Patricia-дереве; возвращает локальный termID или END.
    uint32_t lookup(std::string_view term) const;
    /// @brief Создаёт итератор терма; отсутствие терма даёт пустой итератор.
    /// Сегмент и необязательный счётчик stats должны существовать дольше итератора.
    Iter iterator(std::string_view term, SearchStats* stats) const;
    /// @brief Возвращает число записей термов в словаре этого сегмента.
    uint64_t vocabulary_size() const { return terms.size() / 40; }
};
class Index {
    MappedFile docs_data_, docs_offsets_;

  public:
    uint32_t documents = 0;
    uint64_t token_count = 0;
    std::vector<std::unique_ptr<Segment>> segments;
    /// @brief Открывает готовый индекс, проверяя формат, токенизатор и диапазоны сегментов.
    explicit Index(const fs::path& root);
    /// @brief Читает метаданные по глобальному docID; отклоняет ID вне индекса.
    DocumentInfo document(uint32_t doc) const;
};
/// @brief Создаёт итератор без документов, с нулевой стоимостью и пустыми координатами.
Iter empty_iterator();
} // namespace l0
