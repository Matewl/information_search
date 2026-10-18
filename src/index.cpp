#include "l0/index.hpp"
#include "l0/codec.hpp"
#include "l0/tokenizer.hpp"
#include <cmath>
#include <iostream>
#include <numeric>

namespace l0 {
namespace {
// Дополнительный бит перед каждым байтом отличает конец ключа от его продолжения.
// Благодаря этому ключи «a» и «ab» могут быть разными листьями битового дерева.
/// @brief Возвращает бит префиксно-свободного представления UTF-8 ключа.
/// Каждый байт предваряется единицей, за концом строки возвращаются нули.
bool key_bit(std::string_view s, uint32_t bit) {
    size_t i = bit / 9;
    unsigned j = bit % 9;
    if (i >= s.size())
        return false;
    return j == 0 || ((uint8_t(s[i]) >> (8 - j)) & 1);
}
struct TrieNode {
    uint32_t bit = END, left = END, right = END, term = END;
};
/// @brief Строит Patricia-поддерево для непустого диапазона отсортированных ключей [lo, hi).
/// Добавляет узлы в nodes и возвращает индекс корня; листья используют глобальные индексы keys.
uint32_t build_trie(const std::vector<std::string_view>& keys, size_t lo, size_t hi,
                    std::vector<TrieNode>& nodes) {
    require(nodes.size() < END, "dictionary too large");
    uint32_t id = uint32_t(nodes.size());
    nodes.push_back({});
    if (hi - lo == 1) {
        nodes[id].term = uint32_t(lo);
        return id;
    }
    // У отсортированного диапазона общий префикс определяется крайними ключами.
    auto a = keys[lo], b = keys[hi - 1];
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common])
        ++common;
    require(common < UINT32_MAX / 9, "term too long");
    uint32_t bit = uint32_t(common * 9);
    while (key_bit(a, bit) == key_bit(b, bit))
        ++bit;
    // По первому различающему биту двоичным поиском делим диапазон на две ветви.
    size_t l = lo, r = hi;
    while (l < r) {
        size_t mid = l + (r - l) / 2;
        if (key_bit(keys[mid], bit))
            r = mid;
        else
            l = mid + 1;
    }
    require(l > lo && l < hi, "invalid radix split");
    auto left = build_trie(keys, lo, l, nodes), right = build_trie(keys, l, hi, nodes);
    nodes[id] = {bit, left, right, END};
    return id;
}
struct BlockMeta {
    uint32_t last, count;
    uint64_t offset;
};
class Empty final : public Iterator {
    std::vector<Span> none_;

  public:
    /// @brief Возвращает END: пустой итератор изначально исчерпан.
    uint32_t doc() const override { return END; }
    /// @brief Сохраняет исчерпанное состояние независимо от цели продвижения.
    uint32_t advance(uint32_t) override { return END; }
    /// @brief Возвращает нулевую оценку числа документов.
    uint64_t cost() const override { return 0; }
    /// @brief Возвращает пустой список координат пустого итератора.
    const std::vector<Span>& spans() override { return none_; }
};
} // namespace
/// @brief Создаёт объект пустого итератора для отсутствующего терма.
Iter empty_iterator() {
    return std::make_unique<Empty>();
}

/// @brief Создаёт хранилище метаданных и рабочее состояние SPIMI в новом каталоге.
Builder::Builder(const fs::path& root, uint64_t memory_bytes) : root_(root), budget_(memory_bytes) {
    require(memory_bytes > 0, "memory budget must be positive");
    require(!fs::exists(root),
            "output already exists; choose a new index directory: " + root.string());
    fs::create_directories(root);
    docs_data_.open(root / "documents.bin", std::ios::binary);
    docs_offsets_.open(root / "documents.idx", std::ios::binary);
    require(bool(docs_data_) && bool(docs_offsets_), "cannot create document store");
}
/// @brief Присваивает следующий docID, записывает метаданные и накапливает posting lists.
/// При достижении порога памяти выгрузка выполняется после обработки всего документа.
void Builder::add(const Document& d) {
    require(!done_ && docs_ < END, "builder finished or document ID limit exceeded");
    require(std::isfinite(d.score), "document score must be finite");
    require(d.score <= last_score_, "documents must be ranked by descending score before indexing");
    last_score_ = d.score;
    Bytes off;
    put64(off, tell(docs_data_));
    write(docs_offsets_, off);
    Bytes metadata;
    string_bytes(metadata, d.id);
    string_bytes(metadata, d.url);
    string_bytes(metadata, d.source);
    put64(metadata, d.row);
    put_double(metadata, d.score);
    write(docs_data_, metadata);
    // Обработчик добавляет координату к posting текущего документа. DocID поступают
    // по возрастанию, поэтому posting lists не требуют дополнительной сортировки.
    tokenize(d.text, [&](std::string_view term, uint32_t pos) {
        auto [it, inserted] = terms_.try_emplace(std::string(term));
        // Учитываем ёмкости векторов и приблизительные накладные расходы словаря,
        // а не только фактическое количество сохранённых элементов.
        if (inserted)
            memory_ += term.size() + 128;
        auto& list = it->second;
        if (list.empty() || list.back().doc != docs_) {
            size_t old = list.capacity();
            list.push_back({docs_, {}});
            memory_ += (list.capacity() - old) * sizeof(Posting);
        }
        auto& positions = list.back().positions;
        size_t old = positions.capacity();
        positions.push_back(pos);
        memory_ += (positions.capacity() - old) * sizeof(uint32_t);
        ++token_count_;
    });
    ++docs_;
    if (memory_ >= budget_)
        flush();
}
/// @brief Сериализует сегмент: сжатые posting lists, skip lists, словарь и Patricia-дерево.
/// После успешной записи освобождает накопленные термы; пустой диапазон пропускается.
void Builder::flush() {
    if (docs_ == segment_start_)
        return;
    std::string name = "segment-" + std::to_string(segments_.size());
    /// @brief Открывает бинарный выходной файл текущего сегмента с заданным суффиксом.
    auto file = [&](std::string_view suffix) {
        return std::ofstream(root_ / (name + std::string(suffix)), std::ios::binary);
    };
    auto tf = file(".terms"), kf = file(".keys"), tr = file(".trie"), pf = file(".post"),
         sf = file(".skip");
    // Представления ключей действуют до очистки terms_ в конце выгрузки.
    std::vector<std::string_view> keys;
    keys.reserve(terms_.size());
    for (auto& item : terms_)
        keys.push_back(item.first);
    std::sort(keys.begin(), keys.end());
    require(keys.size() < END, "too many terms in segment");
    for (auto key : keys) {
        auto& list = terms_.find(std::string(key))->second;
        uint64_t po = tell(pf), so = tell(sf), ko = tell(kf);
        kf.write(key.data(), key.size());
        require(bool(kf), "key write failed");
        std::vector<BlockMeta> blocks;
        for (size_t start = 0; start < list.size(); start += BLOCK) {
            size_t n = std::min<size_t>(BLOCK, list.size() - start);
            std::vector<uint32_t> gaps, tfreq, pos;
            gaps.reserve(n);
            tfreq.reserve(n);
            // В каждом блоке первый docID абсолютный: любой блок декодируется отдельно.
            uint32_t previous = 0;
            for (size_t j = 0; j < n; ++j) {
                auto& p = list[start + j];
                gaps.push_back(p.doc - previous);
                previous = p.doc;
                require(p.positions.size() <= UINT32_MAX, "position count overflow");
                tfreq.push_back(uint32_t(p.positions.size()));
                // База разностей координат сбрасывается для каждого документа.
                uint32_t last = 0;
                for (uint32_t v : p.positions) {
                    pos.push_back(v - last);
                    last = v;
                }
            }
            Bytes db, fb, pb;
            p4::encode_stream(gaps, db);
            p4::encode_stream(tfreq, fb);
            p4::encode_stream(pos, pb);
            require(pb.size() <= UINT32_MAX, "posting block too large");
            blocks.push_back({list[start + n - 1].doc, uint32_t(n), tell(pf)});
            Bytes header;
            put32(header, uint32_t(n));
            put32(header, uint32_t(db.size()));
            put32(header, uint32_t(fb.size()));
            put32(header, uint32_t(pb.size()));
            write(pf, header);
            write(pf, db);
            write(pf, fb);
            write(pf, pb);
        }
        // Базовый уровень адресует каждый блок. Верхние уровни — концы групп
        // из 8, 64, 512, … блоков; связи вычисляются по индексам массивов.
        uint32_t nb = uint32_t(blocks.size()), levels = 0;
        for (uint64_t stride = 8; stride <= nb; stride *= 8)
            ++levels;
        Bytes skip;
        put32(skip, nb);
        put32(skip, levels);
        for (auto b : blocks) {
            put32(skip, b.last);
            put32(skip, b.count);
            put64(skip, b.offset);
        }
        uint64_t stride = 8;
        for (uint32_t l = 0; l < levels; ++l, stride *= 8) {
            put32(skip, uint32_t(stride));
            put32(skip, uint32_t(nb / stride));
            for (uint64_t j = stride - 1; j < nb; j += stride) {
                put32(skip, blocks[j].last);
                put32(skip, uint32_t(j));
            }
        }
        write(sf, skip);
        Bytes term;
        put64(term, ko);
        put32(term, uint32_t(key.size()));
        put32(term, uint32_t(list.size()));
        put64(term, po);
        put64(term, so);
        put32(term, nb);
        put32(term, 0);
        write(tf, term);
    }
    std::vector<TrieNode> nodes;
    nodes.reserve(keys.size() * 2);
    if (!keys.empty())
        build_trie(keys, 0, keys.size(), nodes);
    Bytes trie;
    trie.reserve(nodes.size() * 16);
    for (auto n : nodes) {
        put32(trie, n.bit);
        put32(trie, n.left);
        put32(trie, n.right);
        put32(trie, n.term);
    }
    write(tr, trie);
    for (auto* out : {&tf, &kf, &tr, &pf, &sf}) {
        out->close();
        require(bool(*out), "segment close failed");
    }
    segments_.push_back({name, segment_start_, docs_ - segment_start_});
    segment_start_ = docs_;
    std::cerr << "flushed " << name << ": documents=" << segments_.back().count
              << " terms=" << keys.size() << "\n";
    terms_.clear();
    terms_.rehash(0);
    memory_ = 0;
}
/// @brief Закрывает данные и публикует manifest только после записи всех сегментов.
void Builder::finish() {
    require(!done_, "builder already finished");
    flush();
    Bytes end;
    put64(end, tell(docs_data_));
    write(docs_offsets_, end);
    docs_data_.close();
    docs_offsets_.close();
    require(bool(docs_data_) && bool(docs_offsets_), "document store close failed");
    std::ofstream manifest(root_ / "manifest.tmp");
    manifest << "L0INDEX 2\n"
             << tokenizer_version() << "\n"
             << "score-desc-stable-v1\n"
             << BLOCK << " " << docs_ << " " << token_count_ << " " << segments_.size() << "\n";
    for (auto& s : segments_)
        manifest << s.name << " " << s.first << " " << s.count << "\n";
    manifest.close();
    require(bool(manifest), "manifest write failed");
    // Читатель увидит готовый манифест только после успешного закрытия файлов.
    // Это публикация завершённой сборки, но не гарантия сохранности при потере питания.
    fs::rename(root_ / "manifest.tmp", root_ / "manifest");
    done_ = true;
}

/// @brief Отображает пять файлов сегмента и проверяет размеры записей словаря и дерева.
Segment::Segment(const fs::path& p, uint32_t f, uint32_t c)
    : terms(p.string() + ".terms"), keys(p.string() + ".keys"), trie(p.string() + ".trie"),
      postings(p.string() + ".post"), skips(p.string() + ".skip"), first(f), count(c) {
    require(terms.size() % 40 == 0 && trie.size() % 16 == 0, "invalid dictionary size");
    require(trie.size() / 16 == (vocabulary_size() ? 2 * vocabulary_size() - 1 : 0),
            "invalid trie size");
}
/// @brief Обходит Patricia-дерево по битам ключа и проверяет совпадение строки в листе.
/// Для отсутствующего терма возвращает END, для повреждённого дерева выбрасывает исключение.
uint32_t Segment::lookup(std::string_view key) const {
    if (!trie.size())
        return END;
    uint32_t node = 0, last = 0;
    bool initial = true;
    for (;;) {
        auto p = trie.at(uint64_t(node) * 16, 16);
        uint32_t bit = get32(p);
        if (bit == END) {
            auto id = get32(p + 12);
            auto t = terms.at(uint64_t(id) * 40, 40);
            auto n = get32(t + 8);
            auto str = keys.at(get64(t), n);
            // Путь по различающим битам выбирает кандидата; полная проверка обязательна.
            return key.size() == n && std::memcmp(str, key.data(), n) == 0 ? id : END;
        }
        require(initial || bit > last, "cyclic/corrupt trie");
        last = bit;
        initial = false;
        node = get32(p + (key_bit(key, bit) ? 8 : 4));
    }
}
namespace {
class TermIterator final : public Iterator {
    const Segment& segment_;
    SearchStats* stats_;
    uint32_t df_, nb_, block_ = END, doc_ = END;
    const uint8_t* blocks_;
    struct Level {
        uint32_t stride, count;
        const uint8_t* entries;
    };
    std::vector<Level> levels_;
    std::vector<uint32_t> docs_, freq_, positions_;
    std::vector<size_t> starts_;
    size_t item_ = 0;
    bool positions_loaded_ = false;
    std::vector<Span> spans_;
    /// @brief Читает максимальный docID блока из базового уровня skip list без распаковки.
    uint32_t last(uint32_t block) const { return get32(blocks_ + uint64_t(block) * 16); }
    /// @brief Загружает docID блока b, восстанавливает разности и сбрасывает кеш координат.
    /// Потоки частот и позиций на этом этапе не распаковываются.
    void load(uint32_t b) {
        block_ = b;
        auto meta = blocks_ + uint64_t(b) * 16;
        auto c = segment_.postings.cursor(get64(meta + 8));
        uint32_t n = c.u32(), bytes = c.u32();
        require(n == get32(meta + 4) && n > 0 && n <= BLOCK, "invalid posting count");
        // Пропускаем размеры TF и координат: обычному булеву запросу они пока не нужны.
        c.take(8);
        auto p = c.take(bytes);
        docs_ = p4::decode_stream({p, bytes}, n);
        uint64_t sum = 0;
        for (size_t i = 0; i < docs_.size(); ++i) {
            require(i == 0 || docs_[i] > 0, "unsorted postings");
            sum += docs_[i];
            require(sum < END, "doc ID overflow");
            docs_[i] = uint32_t(sum);
        }
        require(docs_.back() == last(b) && docs_.front() >= segment_.first &&
                    docs_.back() < uint64_t(segment_.first) + segment_.count,
                "invalid document range");
        item_ = 0;
        positions_loaded_ = false;
        spans_.clear();
        if (stats_)
            ++stats_->blocks_decoded;
    }
    /// @brief Лениво распаковывает частоты и координаты текущего блока.
    /// Префиксные суммы частот задают диапазон позиций каждого документа.
    void load_positions() {
        auto meta = blocks_ + uint64_t(block_) * 16;
        auto c = segment_.postings.cursor(get64(meta + 8));
        auto n = c.u32(), db = c.u32(), fb = c.u32(), pb = c.u32();
        c.take(db);
        auto fp = c.take(fb);
        freq_ = p4::decode_stream({fp, fb}, n);
        // starts_[i] и starts_[i+1] ограничивают позиции i-го документа в общем потоке.
        starts_.assign(1, 0);
        for (uint32_t f : freq_) {
            require(f > 0 && f <= SIZE_MAX - starts_.back(), "invalid term frequency");
            starts_.push_back(starts_.back() + f);
        }
        auto pp = c.take(pb);
        positions_ = p4::decode_stream({pp, pb}, starts_.back());
        for (size_t i = 0; i < freq_.size(); ++i) {
            uint64_t sum = 0;
            for (size_t j = starts_[i]; j < starts_[i + 1]; ++j) {
                require(j == starts_[i] || positions_[j] > 0, "unsorted positions");
                sum += positions_[j];
                require(sum < END, "position overflow");
                positions_[j] = uint32_t(sum);
            }
        }
        positions_loaded_ = true;
        if (stats_)
            ++stats_->position_blocks_decoded;
    }

  public:
    /// @brief Открывает skip lists терма и устанавливает итератор на первый документ.
    TermIterator(const Segment& s, uint32_t term, SearchStats* stats) : segment_(s), stats_(stats) {
        auto t = s.terms.at(uint64_t(term) * 40, 40);
        df_ = get32(t + 12);
        auto c = s.skips.cursor(get64(t + 24));
        nb_ = c.u32();
        uint32_t levels = c.u32();
        require(nb_ > 0 && nb_ == get32(t + 32) && levels <= 11, "invalid skip header");
        blocks_ = c.take(uint64_t(nb_) * 16);
        uint64_t stride = 8;
        for (uint32_t i = 0; i < levels; ++i, stride *= 8) {
            uint32_t st = c.u32(), n = c.u32();
            require(st == stride && n == nb_ / st && n > 0, "invalid skip level");
            levels_.push_back({st, n, c.take(uint64_t(n) * 8)});
        }
        load(0);
        doc_ = docs_[0];
    }
    /// @brief Возвращает текущий docID терма либо END после исчерпания.
    uint32_t doc() const override { return doc_; }
    /// @brief Возвращает точную document frequency терма внутри сегмента.
    uint64_t cost() const override { return df_; }
    /// @brief Находит первый доступный docID >= target через skip lists и поиск в блоке.
    /// Не возвращается назад и не распаковывает пропущенные блоки.
    uint32_t advance(uint32_t target) override {
        if (doc_ == END || target <= doc_)
            return doc_;
        spans_.clear();
        if (target == END) {
            doc_ = END;
            return doc_;
        }
        if (last(block_) < target) {
            uint32_t candidate = block_ + 1;
            // Идём от редких ссылок к частым и пропускаем только группы,
            // максимальный docID которых строго меньше цели.
            for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
                uint32_t k = candidate / it->stride;
                while (k < it->count && get32(it->entries + uint64_t(k) * 8) < target) {
                    auto end = get32(it->entries + uint64_t(k) * 8 + 4);
                    require(end == uint64_t(k + 1) * it->stride - 1, "invalid skip link");
                    candidate = end + 1;
                    ++k;
                    if (stats_)
                        ++stats_->skip_jumps;
                }
            }
            while (candidate < nb_ && last(candidate) < target) {
                ++candidate;
                if (stats_)
                    ++stats_->skip_jumps;
            }
            if (candidate == nb_) {
                doc_ = END;
                return doc_;
            }
            load(candidate);
        }
        // После выбора блока нужный docID ищется только в его распакованном массиве.
        auto it = std::lower_bound(docs_.begin() + item_, docs_.end(), target);
        require(it != docs_.end(), "invalid skip order");
        item_ = size_t(it - docs_.begin());
        doc_ = *it;
        return doc_;
    }
    /// @brief Возвращает одноточечные интервалы всех вхождений терма в текущем документе.
    /// Координаты блока загружаются при первом обращении; результат кешируется до продвижения.
    const std::vector<Span>& spans() override {
        if (doc_ == END) {
            spans_.clear();
            return spans_;
        }
        if (!positions_loaded_)
            load_positions();
        if (spans_.empty()) {
            spans_.reserve(freq_[item_]);
            for (size_t i = starts_[item_]; i < starts_[item_ + 1]; ++i)
                spans_.push_back({positions_[i], positions_[i]});
        }
        return spans_;
    }
};
} // namespace
/// @brief Создаёт posting-итератор найденного терма либо пустой итератор.
Iter Segment::iterator(std::string_view term, SearchStats* stats) const {
    auto id = lookup(term);
    return id == END ? empty_iterator() : std::make_unique<TermIterator>(*this, id, stats);
}

/// @brief Открывает опубликованный индекс и проверяет формат, токенизацию и непрерывность docID.
Index::Index(const fs::path& root)
    : docs_data_(root / "documents.bin"), docs_offsets_(root / "documents.idx") {
    std::ifstream in(root / "manifest");
    std::string magic, tokenizer, ranking;
    unsigned version, block;
    uint64_t nsegments;
    require(bool(in >> magic >> version), "missing/incomplete manifest");
    require(magic == "L0INDEX" && version == 2,
            "unsupported index format; rebuild index with stemming and score ranking");
    require(bool(in >> tokenizer >> ranking >> block >> documents >> token_count >> nsegments),
            "missing/incomplete manifest");
    require(block == BLOCK && ranking == "score-desc-stable-v1", "unsupported index configuration");
    require(tokenizer == tokenizer_version(), "index/query tokenizer versions differ");
    require(docs_offsets_.size() == (uint64_t(documents) + 1) * 8, "invalid document offset table");
    // Непрерывные непересекающиеся диапазоны позволяют выдавать сегменты по очереди.
    uint64_t expected = 0;
    for (uint64_t i = 0; i < nsegments; ++i) {
        std::string name;
        uint32_t first, count;
        require(bool(in >> name >> first >> count), "truncated manifest");
        require(name == "segment-" + std::to_string(i) && first == expected && count > 0,
                "invalid segment range/name");
        segments.push_back(std::make_unique<Segment>(root / name, first, count));
        expected += count;
    }
    require(expected == documents, "document count mismatch");
    require(get64(docs_offsets_.at(uint64_t(documents) * 8, 8)) == docs_data_.size(),
            "invalid document store size");
}
/// @brief Читает ровно одну запись метаданных между соседними смещениями documents.idx.
DocumentInfo Index::document(uint32_t doc) const {
    require(doc < documents, "document ID out of range");
    auto p = docs_offsets_.at(uint64_t(doc) * 8, 16);
    auto a = get64(p), b = get64(p + 8);
    require(a <= b, "invalid metadata offset");
    Cursor c{docs_data_.at(a, b - a), size_t(b - a)};
    DocumentInfo d;
    d.id = c.str();
    d.url = c.str();
    d.source = c.str();
    d.row = c.u64();
    d.score = get_double(c.take(8));
    require(std::isfinite(d.score), "invalid stored score");
    require(c.left == 0, "invalid document metadata");
    return d;
}
} // namespace l0
