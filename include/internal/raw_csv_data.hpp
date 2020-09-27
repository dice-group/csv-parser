#pragma once
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../external/mio.hpp"
#include "col_names.hpp"
#include "compatibility.hpp"
#include "csv_row.hpp"

namespace csv {
    namespace internals {
        /**  @typedef ParseFlags
         *   An enum used for describing the significance of each character
         *   with respect to CSV parsing
         */
        enum class ParseFlags {
            NOT_SPECIAL, /**< Characters with no special meaning */
            QUOTE,       /**< Characters which may signify a quote escape */
            DELIMITER,   /**< Characters which may signify a new field */
            NEWLINE      /**< Characters which may signify a new row */
        };

        using ParseFlagMap = std::array<ParseFlags, 256>;
        using WhitespaceMap = std::array<bool, 256>;
    }
    
    template<typename T>
    class ThreadSafeDeque {
    public:
        ThreadSafeDeque() = default;
        ThreadSafeDeque(const ThreadSafeDeque& other) {
            this->data = other.data;
        }

        ThreadSafeDeque(const std::deque<T>& source) {
            this->data = source;
        }

        bool empty() const noexcept {
            
            return this->data.empty();
        }

        T& front() noexcept {
            return this->data.front();
        }

        T& operator[](size_t n) {
            return this->data[n];
        }

        void push_back(T&& item) {
            std::unique_lock<std::mutex> lock{ this->_lock };
            this->data.push_back(std::move(item));
            this->_cond.notify_all();
            lock.unlock();
        }

        bool wait_for_data() {
            std::unique_lock<std::mutex> lock{ this->_lock };
            this->_cond.wait(lock, [this] { return !this->empty() || this->stop_waiting == true; });
            lock.unlock();
            return true;
        }

        T pop_front() noexcept {
            std::unique_lock<std::mutex> lock{ this->_lock };
            T item = std::move(data.front());
            data.pop_front();
            lock.unlock();

            return item;
        }

        size_t size() const noexcept { return this->data.size(); }

        std::deque<CSVRow>::iterator begin() noexcept {
            return this->data.begin();
        }

        std::deque<CSVRow>::iterator end() noexcept {
            return this->data.end();
        }

        void start_waiters() {
            std::unique_lock<std::mutex> lock{ this->_lock };
            this->stop_waiting = false;
            this->_cond.notify_all();
        }

        void stop_waiters() {
            std::unique_lock<std::mutex> lock{ this->_lock };
            this->stop_waiting = true;
            this->_cond.notify_all();
        }

        void clear() noexcept {
            this->data.clear();
        }

        bool stop_waiting = true;

    private:
        std::mutex _lock;
        std::condition_variable _cond;
        std::deque<T> data;
    };

    /** A class for parsing raw CSV data */
    class BasicCSVParser {
        using RowCollection = ThreadSafeDeque<CSVRow>;

    public:
        BasicCSVParser() = default;
        BasicCSVParser(internals::ColNamesPtr _col_names) : col_names(_col_names) {};
        BasicCSVParser(internals::ParseFlagMap parse_flags, internals::WhitespaceMap ws_flags) :
            _parse_flags(parse_flags), _ws_flags(ws_flags) {};

        mio::mmap_source data_source;

        size_t parse(csv::string_view in, RowCollection& records, bool last_block = true);

        void set_parse_flags(internals::ParseFlagMap parse_flags) {
            _parse_flags = parse_flags;
        }

        void set_ws_flags(internals::WhitespaceMap ws_flags) {
            _ws_flags = ws_flags;
        }

    private:
        CONSTEXPR internals::ParseFlags parse_flag(const char ch) const {
            return _parse_flags.data()[ch + 128];
        }

        CONSTEXPR bool ws_flag(const char ch) const {
            return _ws_flags.data()[ch + 128];
        }

        size_t parse_loop(csv::string_view in, bool last_block);

        void push_field(
            CSVRow& row,
            int& field_start,
            size_t& field_length,
            bool& has_double_quote);

        template<bool QuoteEscape=false>
        CONSTEXPR void parse_field(string_view in, size_t& i,
            int& field_start,
            size_t& field_length,
            const size_t& current_row_start) {
            using internals::ParseFlags;

            // Trim off leading whitespace
            while (i < in.size() && ws_flag(in[i])) i++;

            if (field_start < 0)
                field_start = (int)(i - current_row_start);

            // Optimization: Since NOT_SPECIAL characters tend to occur in contiguous
            // sequences, use the loop below to avoid having to go through the outer
            // switch statement as much as possible
            IF_CONSTEXPR(QuoteEscape) {
                while (i < in.size() && parse_flag(in[i]) != ParseFlags::QUOTE) i++;
            }
            else {
                while (i < in.size() && parse_flag(in[i]) == ParseFlags::NOT_SPECIAL) i++;
            }

            field_length = i - (field_start + current_row_start);

            // Trim off trailing whitespace, field_length constraint matters
            // when field is entirely whitespace
            for (size_t j = i - 1; ws_flag(in[j]) && field_length > 0; j--) field_length--;
        }

        void push_row(CSVRow&& row, RowCollection& records) {
            row.row_length = row.data->fields.size() - row.field_bounds_index;
            records.push_back(std::move(row));
        };

        void set_data_ptr(RawCSVDataPtr ptr) {
            this->data_ptr = ptr;
            this->fields = &(ptr->fields);
        }

        /** An array where the (i + 128)th slot gives the ParseFlags for ASCII character i */
        internals::ParseFlagMap _parse_flags;

        /** An array where the (i + 128)th slot determines whether ASCII character i should
         *  be trimmed
         */
        internals::WhitespaceMap _ws_flags;

        internals::ColNamesPtr col_names = nullptr;

        RawCSVDataPtr data_ptr = nullptr;
        internals::CSVFieldArray* fields = nullptr;
        RowCollection* _records = nullptr;
    };
}