#pragma once
#include "index.hpp"
namespace l0 {
/// @brief Разрешает путь среза и возвращает отсортированные абсолютные пути Parquet-файлов.
/// При отсутствии подходящих файлов выбрасывает исключение.
std::vector<fs::path> corpus_files(fs::path input);
/// @brief Читает срез, сортирует документы по score и затем добавляет их в пустой builder.
/// max_docs ограничивает исходный префикс корпуса до сортировки; 0 означает весь срез.
/// scratch — новый временный каталог; sort_memory_bytes — приблизительный порог порции.
/// batch_size должен быть положительным; builder.finish() выполняет вызывающий код.
void ingest_parquet(Builder& builder, const fs::path& input, const fs::path& scratch,
                    uint64_t max_docs = 0, int64_t batch_size = 256,
                    uint64_t sort_memory_bytes = 256ull * 1024 * 1024);
} // namespace l0
