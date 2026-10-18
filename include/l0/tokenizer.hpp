#pragma once
#include "common.hpp"
#include <functional>
namespace l0 {
// Документы и запросы используют одинаковые правила регистра и границ токенов.
/// @brief Выполняет Unicode case folding и Snowball English, передавая термы с координатами в emit.
/// Координаты начинаются с нуля; представление токена действительно только во время emit.
/// Стоп-слова сохраняются; стемминг применяется к токенам a-z, остальные сохраняются.
void tokenize(std::string_view text, const std::function<void(std::string_view, uint32_t)>& emit);
/// @brief Возвращает нормализованные токены текста как независимые строки.
std::vector<std::string> tokens(std::string_view text);
/// @brief Возвращает идентификатор алгоритма и версии Unicode для проверки совместимости индекса.
std::string tokenizer_version();
} // namespace l0
