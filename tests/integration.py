#!/usr/bin/env python3
"""Проверки Parquet и CLI, а также независимый эталон поиска по реальному FineWeb."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unicodedata
import math
from functools import lru_cache
import snowballstemmer
import pyarrow as pa
import pyarrow.parquet as pq

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/release/l0"
STEMMER = snowballstemmer.stemmer("english")


@lru_cache(maxsize=200000)
def stem(word):
    """Применяет независимый Python Snowball 3.0.1 к токенам a-z, как C++ обёртка."""
    return STEMMER.stemWord(word) if word.isascii() and word.isalpha() else word


def run(*args, ok=True):
    """Запускает CLI и проверяет ожидаемый успех или отказ.

    args передаются как отдельные аргументы без shell. При ok=False ожидается
    ненулевой код завершения. Возвращает CompletedProcess с stdout и stderr.
    """
    proc = subprocess.run([str(BIN), *map(str, args)], text=True, capture_output=True)
    if ok and proc.returncode:
        raise AssertionError(proc.stderr)
    if not ok:
        assert proc.returncode != 0, proc.stdout
    return proc


def query(index, expression):
    """Возвращает JSON-результат полного поиска expression в каталоге index.

    Подсчёт полный; число возвращаемых документов ограничено 100 000,
    чего достаточно для предусмотренных тестовых корпусов.
    """
    return json.loads(run("search", "--index", index, "--query", expression,
                          "--count", "--limit", "100000", "--json").stdout)


def tokenize(text):
    """Возвращает токены независимой реализации case folding и Snowball English.

    Буквы и десятичные цифры образуют слово; комбинируемые знаки допускаются
    только после начала слова. Правила соответствуют токенизатору движка.
    """
    result, word = [], []
    for char in text.casefold():
        cat = unicodedata.category(char)
        if cat.startswith("L") or cat == "Nd" or (word and cat.startswith("M")):
            word.append(char)
        elif word:
            result.append("".join(word))
            word = []
    if word:
        result.append("".join(word))
    return [stem(word) for word in result]


def fixture_tests(temp):
    """Проверяет чтение Parquet, запросы, CLI и ошибки на небольшом корпусе.

    temp — существующий временный каталог. Все создаваемые файлы,
    включая намеренно повреждённый индекс, остаются внутри него.
    """
    source = temp / "input"
    source.mkdir()
    # Два файла и маленькие группы строк проверяют переходы между пакетами и файлами.
    for shard, texts in enumerate([["Alpha beta", "beta alpha", None],
                                    ["alpha x beta", "Straße ЁЖ", "alpha alpha"]]):
        table = pa.table({"text": texts, "id": [f"{shard}-{i}" for i in range(3)],
                          "url": ["https://example.test/\"quote" for _ in texts],
                          "dump": ["CC-MAIN-2014-10"] * 3,
                          "score": [[1.0, 3.5, 0.0], [2.0, 3.5, 4.0]][shard]})
        pq.write_table(table, source / f"part-{shard}.parquet", row_group_size=2)
    index = temp / "index"
    run("build", "--input", source, "--output", index, "--batch-size", "1", "--memory-mb", "1")
    # Ожидаемые docID заданы вручную: эталон не зависит от реализации C++ поиска.
    # Глобальный порядок исходных строк: 5, 1, 4, 3, 0, 2; равные score стабильны.
    cases = {"alpha": [0, 1, 3, 4], '"alpha beta"': [4],
             "alpha NEAR/1 beta": [1, 4], "alpha ADJ/2 beta": [3, 4],
             "alpha AND NOT beta": [0], "strasse AND ёж": [2],
             "alpha NEAR/1 alpha": [0], '"alpha alpha"': [0]}
    for expression, expected in cases.items():
        result = query(index, expression)
        assert [h["doc_id"] for h in result["hits"]] == expected, (expression, result)
        assert result["complete"]
        assert [h["score"] for h in result["hits"]] == sorted(
            [h["score"] for h in result["hits"]], reverse=True)
    info = json.loads(run("document", "--index", index, "--doc-id", 2).stdout)
    assert info["id"] == "1-1" and info["row"] == 1 and info["source"].endswith("part-1.parquet")
    assert info["score"] == 3.5
    assert not (index / ".ranking").exists()
    top = json.loads(run("search", "--index", index, "--query", "alpha", "--limit", 1, "--json").stdout)
    assert top["hits"][0]["id"] == "1-2" and top["hits"][0]["score"] == 4.0
    limited = temp / "limited"
    run("build", "--input", source, "--output", limited, "--max-docs", 4)
    assert json.loads(run("stats", "--index", limited).stdout)["documents"] == 4
    assert [h["id"] for h in query(limited, "alpha")["hits"]] == ["0-1", "1-0", "0-0"]
    run("search", "--index", index, "--query", "a AND (", ok=False)
    run("build", "--input", source, "--output", index, ok=False)
    bad = temp / "bad.parquet"
    pq.write_table(pa.table({"text": ["x"], "id": ["x"], "url": ["x"],
                            "dump": ["CC-MAIN-2020-05"], "score": [1.0]}), bad)
    run("build", "--input", bad, "--output", temp / "bad-index", ok=False)
    assert not (temp / "bad-index/manifest").exists()
    # Отсутствие или неопределённость score не должны незаметно превращаться в другой ранг.
    for i, value in enumerate([None, float("nan"), float("inf"), float("-inf")]):
        invalid = temp / f"invalid-score-{i}.parquet"
        pq.write_table(pa.table({"text": ["word"], "id": ["x"], "url": ["x"],
                                "score": pa.array([value], type=pa.float64())}), invalid)
        failed = temp / f"invalid-score-index-{i}"
        run("build", "--input", invalid, "--output", failed, ok=False)
        assert not (failed / "manifest").exists() and not (failed / ".ranking").exists()
    absent = temp / "missing-score.parquet"
    pq.write_table(pa.table({"text": ["word"], "id": ["x"], "url": ["x"]}), absent)
    run("build", "--input", absent, "--output", temp / "missing-score-index", ok=False)
    morphology = temp / "morphology.parquet"
    pq.write_table(pa.table({"text": ["Running dogs run", "a runner studies", "a dog runs"],
                            "id": ["low", "high", "middle"], "url": [""] * 3,
                            "score": [0.125, 5.0, 2.25]}), morphology)
    morph_index = temp / "morph-index"
    run("build", "--input", morphology, "--output", morph_index)
    for expression, expected in {"running": ["middle", "low"], "studying": ["high"],
                                 '"run dog"': ["low"], "dogs ADJ running": ["middle", "low"]}.items():
        assert [h["id"] for h in query(morph_index, expression)["hits"]] == expected
    # Повреждение словаря должно вызвать ошибку, а не чтение за границами mmap.
    trie = index / "segment-0.trie"
    trie.write_bytes(trie.read_bytes()[:7])
    run("search", "--index", index, "--query", "alpha", ok=False)
    print("PASS Parquet, CLI, stemming, score ranking, ties, source metadata, limits, invalid scores, corruption")


def real_tests(index, count):
    """Сравнивает полные списки docID с независимым поиском по исходному Parquet.

    index должен содержать первые count исходных документов среза,
    стабильно отсортированные по score по убыванию перед индексацией.
    """
    # Порядок чтения совпадает с C++ загрузчиком: имя файла, затем номер строки.
    documents = []
    for file in sorted((ROOT / "fineweb-edu/data/CC-MAIN-2014-10").glob("*.parquet")):
        row = 0
        for batch in pq.ParquetFile(file).iter_batches(batch_size=256, columns=["text", "id", "score"]):
            for document in batch.to_pylist():
                assert document["score"] is not None and math.isfinite(document["score"])
                documents.append({"words": tokenize(document["text"] or ""), "score": document["score"],
                                  "id": document["id"], "row": row, "source": str(file.resolve())})
                row += 1
                if len(documents) == count:
                    break
            if len(documents) == count:
                break
        if len(documents) == count:
            break
    assert len(documents) == count
    documents.sort(key=lambda d: -d["score"])
    def near(words, a, b, distance, ordered):
        """Проверяет близость разных вхождений a и b перебором пар координат.

        При ordered=True b должно идти после a; иначе допустим любой порядок.
        Разность координат лежит в диапазоне от 1 до distance включительно.
        """
        apos = [i for i, word in enumerate(words) if word == stem(a)]
        bpos = [i for i, word in enumerate(words) if word == stem(b)]
        return any((0 < j-i <= distance) if ordered else (0 < abs(j-i) <= distance)
                   for i in apos for j in bpos)
    # Каждый предикат независимо задаёт семантику соответствующего запроса.
    # Булевы предикаты проверяют наличие слов, позиционные — пары координат.
    cases = {
        "education": lambda w: stem("education") in w,
        "science AND research": lambda w: stem("science") in w and stem("research") in w,
        "education OR research": lambda w: stem("education") in w or stem("research") in w,
        "science AND NOT fiction": lambda w: stem("science") in w and stem("fiction") not in w,
        "(water OR energy) AND NOT oil": lambda w: (stem("water") in w or stem("energy") in w) and stem("oil") not in w,
        "running OR studies": lambda w: stem("running") in w or stem("studies") in w,
        '"united states"': lambda w: near(w, "united", "states", 1, True),
        "climate NEAR/5 change": lambda w: near(w, "climate", "change", 5, False),
        "education ADJ/3 research": lambda w: near(w, "education", "research", 3, True),
        "the NEAR/1 the": lambda w: near(w, "the", "the", 1, False),
    }
    for expression, predicate in cases.items():
        expected = [i for i, doc in enumerate(documents) if predicate(doc["words"])]
        result = query(index, expression)
        actual = [h["doc_id"] for h in result["hits"]]
        # Сравниваются сами упорядоченные списки, а не только количество совпадений.
        assert actual == expected, (expression, len(actual), len(expected), set(actual) ^ set(expected))
        assert result["matches"] == len(expected)
        for hit in result["hits"]:
            doc = documents[hit["doc_id"]]
            assert all(hit[key] == doc[key] for key in ["id", "score", "source", "row"])
        print(f"PASS FineWeb {expression!r}: {len(expected)} matches, {result['elapsed_ms']:.3f} ms")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--real-index", type=Path)
    parser.add_argument("--real-docs", type=int, default=10000)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="l0-integration-") as directory:
        fixture_tests(Path(directory))
    if args.real_index:
        real_tests(args.real_index, args.real_docs)
