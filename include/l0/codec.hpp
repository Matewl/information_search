#pragma once
#include "common.hpp"
namespace l0::p4 {
// PForDelta хранит младшие b бит и исключения вида «индекс, старшие биты».
// Преобразование в разности выполняется отдельно, на уровне docID и координат.
/// @brief Дописывает PForDelta-фрейм из n целых, где 1 <= n <= 128.
/// Вход уже должен содержать нужные разности; delta-преобразование здесь не выполняется.
void encode(const uint32_t* values, size_t n, Bytes& out);
/// @brief Читает один PForDelta-фрейм, продвигая курсор; повреждённые данные отклоняются.
std::vector<uint32_t> decode(Cursor& in);
/// @brief Разбивает значения на фреймы до 128 целых и дописывает их в out.
/// Пустой вход ничего не добавляет в выходной буфер.
void encode_stream(const std::vector<uint32_t>& values, Bytes& out);
/// @brief Распаковывает все фреймы курсора и проверяет точное число значений expected.
/// Курсор передаётся по значению: позиция курсора вызывающего кода не меняется.
std::vector<uint32_t> decode_stream(Cursor in, size_t expected);
} // namespace l0::p4
