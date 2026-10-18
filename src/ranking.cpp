#include "l0/ranking.hpp"
#include <cmath>
#include <iostream>
#include <queue>
#include <zlib.h>

namespace l0 {
/// @brief Создаёт отдельный каталог для временных порций внешней сортировки.
RankedDocuments::RankedDocuments(const fs::path& scratch, uint64_t memory_bytes, size_t fan_in)
    : scratch_(scratch), budget_(memory_bytes), fan_in_(fan_in) {
    require(budget_ > 0 && fan_in_ >= 2, "invalid ranking memory budget or merge fan-in");
    require(!fs::exists(scratch_), "ranking scratch directory already exists");
    require(fs::create_directory(scratch_), "cannot create ranking scratch directory");
}
/// @brief Очищает только принадлежащий сортировщику scratch-каталог, не выбрасывая исключений.
RankedDocuments::~RankedDocuments() {
    std::error_code error;
    fs::remove_all(scratch_, error);
}
/// @brief Задаёт полный порядок: высокий score раньше, при равенстве — меньший ordinal.
bool RankedDocuments::before(const Record& a, const Record& b) {
    return a.score > b.score || (a.score == b.score && a.ordinal < b.ordinal);
}
/// @brief Выделяет новое имя файла порции в собственном временном каталоге.
fs::path RankedDocuments::next_path() {
    return scratch_ / ("run-" + std::to_string(next_run_++) + ".bin");
}
/// @brief Записывает числовой ключ сортировки и сжатый документ без C++ struct padding.
void RankedDocuments::write_record(std::ostream& out, const Record& record) {
    require(record.payload.size() <= UINT32_MAX, "compressed document too large");
    Bytes header;
    put_double(header, record.score);
    put64(header, record.ordinal);
    put32(header, record.raw_size);
    put32(header, uint32_t(record.payload.size()));
    write(out, header);
    write(out, record.payload);
}
/// @brief Читает запись порции и отличает конец файла от усечённого заголовка или данных.
bool RankedDocuments::read_record(std::istream& in, Record& record) {
    uint8_t header[24];
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() == 0 && in.eof())
        return false;
    require(in.gcount() == sizeof(header), "truncated ranking record header");
    record.score = get_double(header);
    record.ordinal = get64(header + 8);
    record.raw_size = get32(header + 16);
    require(std::isfinite(record.score), "invalid ranking score");
    record.payload.resize(get32(header + 20));
    in.read(reinterpret_cast<char*>(record.payload.data()), record.payload.size());
    require(bool(in), "truncated ranking record payload");
    return true;
}
/// @brief Упаковывает документ zlib для экономии временного диска и памяти сортировки.
void RankedDocuments::add(const Document& document) {
    require(!finished_, "ranking already consumed");
    require(std::isfinite(document.score), "score must be a finite number");
    require(count_ < END, "document ID limit exceeded");
    Bytes raw;
    string_bytes(raw, document.id);
    string_bytes(raw, document.url);
    string_bytes(raw, document.source);
    put64(raw, document.row);
    string_bytes(raw, document.text);
    require(raw.size() <= UINT32_MAX, "document too large for ranking spool");
    uLongf size = compressBound(raw.size());
    Bytes packed(size);
    require(compress2(packed.data(), &size, raw.data(), raw.size(), Z_BEST_SPEED) == Z_OK,
            "document compression failed");
    packed.resize(size);
    // Удаляем запас compressBound: иначе память порции зависела бы от несжатого текста.
    packed.shrink_to_fit();
    memory_ += packed.capacity() + 2 * sizeof(Record);
    pending_.push_back({document.score, count_++, uint32_t(raw.size()), std::move(packed)});
    if (memory_ >= budget_)
        flush();
}
/// @brief Сортирует одну ограниченную по памяти порцию и сохраняет её для будущего слияния.
void RankedDocuments::flush() {
    if (pending_.empty())
        return;
    std::sort(pending_.begin(), pending_.end(), before);
    auto path = next_path();
    std::ofstream out(path, std::ios::binary);
    require(bool(out), "cannot create ranking run");
    for (const auto& record : pending_)
        write_record(out, record);
    out.close();
    require(bool(out), "cannot close ranking run (disk full?)");
    runs_.push_back(path);
    std::vector<Record>().swap(pending_);
    memory_ = 0;
}
/// @brief Выполняет k-путевое слияние по одной текущей сжатой записи каждого потока.
void RankedDocuments::merge(const std::vector<fs::path>& paths,
                            const std::function<void(const Record&)>& emit) {
    require(paths.size() <= fan_in_, "too many ranking merge inputs");
    std::vector<std::ifstream> inputs(paths.size());
    std::vector<Record> heads(paths.size());
    // priority_queue выводит лучшую запись; сами payload остаются в массиве heads.
    auto worse = [&](size_t a, size_t b) { return before(heads[b], heads[a]); };
    std::priority_queue<size_t, std::vector<size_t>, decltype(worse)> heap(worse);
    for (size_t i = 0; i < paths.size(); ++i) {
        inputs[i].open(paths[i], std::ios::binary);
        require(bool(inputs[i]), "cannot open ranking run");
        require(read_record(inputs[i], heads[i]), "unexpected empty ranking run");
        heap.push(i);
    }
    while (!heap.empty()) {
        size_t i = heap.top();
        heap.pop();
        emit(heads[i]);
        if (read_record(inputs[i], heads[i])) {
            heap.push(i);
        } else {
            inputs[i].close();
            // При финальном слиянии это освобождает место по мере роста самого индекса.
            fs::remove(paths[i]);
        }
    }
}
/// @brief Восстанавливает исходный документ из временной сжатой записи без изменения score.
Document RankedDocuments::unpack(const Record& record) {
    Bytes raw(record.raw_size);
    uLongf size = raw.size();
    require(uncompress(raw.data(), &size, record.payload.data(), record.payload.size()) == Z_OK &&
                size == raw.size(),
            "ranking document decompression failed");
    Cursor cursor{raw.data(), raw.size()};
    Document document;
    document.id = cursor.str();
    document.url = cursor.str();
    document.source = cursor.str();
    document.row = cursor.u64();
    document.text = cursor.str();
    document.score = record.score;
    require(cursor.left == 0, "invalid ranking document");
    return document;
}
/// @brief Сливает отсортированные порции и только затем передаёт документы на индексацию.
void RankedDocuments::index_into(Builder& builder) {
    require(!finished_ && builder.document_count() == 0, "ranking requires a fresh builder");
    flush();
    std::cerr << "ranking documents=" << count_ << " runs=" << runs_.size() << '\n';
    // Ограничение fan-in защищает число открытых файлов и память голов потоков.
    while (runs_.size() > fan_in_) {
        std::vector<fs::path> next;
        for (size_t i = 0; i < runs_.size(); i += fan_in_) {
            auto end = std::min(runs_.size(), i + fan_in_);
            std::vector<fs::path> group(runs_.begin() + i, runs_.begin() + end);
            auto path = next_path();
            std::ofstream out(path, std::ios::binary);
            require(bool(out), "cannot create merged ranking run");
            merge(group, [&](const Record& record) { write_record(out, record); });
            out.close();
            require(bool(out), "cannot close merged ranking run");
            next.push_back(path);
        }
        runs_ = std::move(next);
    }
    merge(runs_, [&](const Record& record) {
        builder.add(unpack(record));
        if (builder.document_count() % 10000 == 0)
            std::cerr << "indexed=" << builder.document_count()
                      << " tokens=" << builder.token_count() << '\n';
    });
    runs_.clear();
    require(builder.document_count() == count_, "ranking lost documents");
    finished_ = true;
}
} // namespace l0
