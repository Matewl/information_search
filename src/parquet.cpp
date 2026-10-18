#include "l0/parquet.hpp"
#include "l0/ranking.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <cmath>
#include <iostream>
#include <numeric>
#include <parquet/arrow/reader.h>

namespace l0 {
namespace {
/// @brief Извлекает значение Arrow Result с перемещением; ошибку преобразует в исключение.
template <class T> T unwrap(arrow::Result<T> result) {
    require(result.ok(), result.status().ToString());
    return std::move(result).ValueOrDie();
}
/// @brief Преобразует неуспешный Arrow Status в исключение с исходным сообщением.
void check(const arrow::Status& status) {
    require(status.ok(), status.ToString());
}
/// @brief Возвращает представление UTF-8 ячейки STRING/LARGE_STRING; null даёт пустую строку.
/// Представление действительно, пока жив соответствующий Arrow-массив.
std::string_view value(const std::shared_ptr<arrow::Array>& array, int64_t row) {
    if (array->IsNull(row))
        return {};
    if (array->type_id() == arrow::Type::STRING)
        return static_cast<const arrow::StringArray&>(*array).GetView(row);
    if (array->type_id() == arrow::Type::LARGE_STRING)
        return static_cast<const arrow::LargeStringArray&>(*array).GetView(row);
    throw std::runtime_error("expected UTF-8 Parquet column");
}
/// @brief Читает исходный вещественный score; null, NaN, бесконечности и другие типы отклоняются.
double score_value(const std::shared_ptr<arrow::Array>& array, int64_t row) {
    require(!array->IsNull(row), "score must not be null");
    double score;
    if (array->type_id() == arrow::Type::DOUBLE)
        score = static_cast<const arrow::DoubleArray&>(*array).Value(row);
    else if (array->type_id() == arrow::Type::FLOAT)
        score = static_cast<const arrow::FloatArray&>(*array).Value(row);
    else
        throw std::runtime_error("score must be a floating-point Parquet column");
    require(std::isfinite(score), "score must be a finite number");
    return score;
}
} // namespace
/// @brief Находит Parquet-файлы указанного среза и сортирует их для стабильной нумерации docID.
std::vector<fs::path> corpus_files(fs::path input) {
    if (fs::is_directory(input / "data" / "CC-MAIN-2014-10"))
        input /= fs::path("data") / "CC-MAIN-2014-10";
    else if (fs::is_directory(input / "CC-MAIN-2014-10"))
        input /= "CC-MAIN-2014-10";
    std::vector<fs::path> files;
    if (fs::is_regular_file(input) && input.extension() == ".parquet")
        files.push_back(fs::absolute(input));
    else {
        require(fs::is_directory(input), "Parquet input not found: " + input.string());
        for (auto& entry : fs::directory_iterator(input))
            if (entry.is_regular_file() && entry.path().extension() == ".parquet")
                files.push_back(fs::absolute(entry.path()));
    }
    std::sort(files.begin(), files.end());
    require(!files.empty(), "no Parquet files found");
    return files;
}
/// @brief Читает исходный префикс корпуса, внешне сортирует по score и затем строит индекс.
/// Проверяет dump при наличии колонки; max_docs=0 означает весь срез.
void ingest_parquet(Builder& builder, const fs::path& input, const fs::path& scratch,
                    uint64_t max_docs, int64_t batch_size, uint64_t sort_memory_bytes) {
    require(batch_size > 0, "batch size must be positive");
    require(builder.document_count() == 0, "ranked ingestion requires an empty builder");
    RankedDocuments ranked(scratch, sort_memory_bytes);
    uint64_t selected = 0;
    for (auto& file : corpus_files(input)) {
        if (max_docs && selected >= max_docs)
            break;
        auto stream = unwrap(arrow::io::ReadableFile::Open(file.string()));
        auto reader = unwrap(parquet::arrow::OpenFile(stream, arrow::default_memory_pool()));
        reader->set_batch_size(batch_size);
        reader->set_use_threads(true);
        std::shared_ptr<arrow::Schema> schema;
        check(reader->GetSchema(&schema));
        // Схема FineWeb плоская: проекция исключает ненужные для индекса колонки.
        std::vector<int> columns;
        std::vector<std::string> names = {"text", "id", "url", "score"};
        for (auto& name : names) {
            int i = schema->GetFieldIndex(name);
            require(i >= 0, "missing Parquet field: " + name);
            columns.push_back(i);
        }
        int dump = schema->GetFieldIndex("dump");
        if (dump >= 0)
            columns.push_back(dump);
        std::vector<int> groups(reader->num_row_groups());
        std::iota(groups.begin(), groups.end(), 0);
        // Reader выдаёт пакеты из групп строк последовательно, без загрузки всего файла.
        auto batches = unwrap(reader->GetRecordBatchReader(groups, columns));
        std::cerr << "reading " << file.filename().string() << "\n";
        // Номер строки относится ко всему файлу, а не к отдельному пакету Arrow.
        uint64_t row = 0;
        while (!max_docs || selected < max_docs) {
            std::shared_ptr<arrow::RecordBatch> batch;
            check(batches->ReadNext(&batch));
            if (!batch)
                break;
            auto text = batch->GetColumnByName("text"), id = batch->GetColumnByName("id"),
                 url = batch->GetColumnByName("url"), dump_col = batch->GetColumnByName("dump"),
                 score = batch->GetColumnByName("score");
            require(text && id && url && score, "missing projected columns");
            for (int64_t i = 0; i < batch->num_rows() && (!max_docs || selected < max_docs);
                 ++i, ++row) {
                if (dump_col)
                    require(value(dump_col, i) == "CC-MAIN-2014-10",
                            "input contains a different FineWeb snapshot");
                // Копируем представления Arrow: следующий пакет может освободить их память.
                Document d;
                d.text = value(text, i);
                d.id = value(id, i);
                d.url = value(url, i);
                d.source = file.string();
                d.row = row;
                d.score = score_value(score, i);
                ranked.add(d);
                ++selected;
                if (selected % 10000 == 0)
                    std::cerr << "selected=" << selected << " for score ranking\n";
            }
        }
    }
    // Только глобальное слияние определяет docID: posting lists сразу строятся в ранговом порядке.
    ranked.index_into(builder);
}
} // namespace l0
