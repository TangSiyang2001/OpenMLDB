/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "storage/aggregator.h"

#include <algorithm>
#include <utility>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "base/file_util.h"
#include "base/glog_wapper.h"
#include "base/slice.h"
#include "base/strings.h"
#include "boost/algorithm/string.hpp"
#include "common/timer.h"
#include "storage/table.h"

DECLARE_bool(binlog_notify_on_put);
namespace openmldb {
namespace storage {

using ::openmldb::base::StringCompare;

std::string AggrStatToString(AggrStat type) {
    std::string output;
    switch (type) {
        case AggrStat::kUnInit:
            output = "UnInit";
            break;
        case AggrStat::kRecovering:
            output = "Recovering";
            break;
        case AggrStat::kInited:
            output = "Inited";
            break;
        default:
            output = "Unknown";
            break;
    }
    return output;
}

Aggregator::Aggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                       std::shared_ptr<Table> aggr_table, std::shared_ptr<LogReplicator> aggr_replicator,
                       const uint32_t& index_pos, const std::string& aggr_col, const AggrType& aggr_type,
                       const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : base_table_schema_(base_meta.column_desc()),
      aggr_table_schema_(aggr_meta.column_desc()),
      aggr_table_(aggr_table),
      aggr_replicator_(aggr_replicator),
      status_(AggrStat::kUnInit),
      index_pos_(index_pos),
      aggr_col_(aggr_col),
      aggr_type_(aggr_type),
      ts_col_(ts_col),
      aggr_col_idx_(-1),
      ts_col_idx_(-1),
      filter_col_idx_(-1),
      window_type_(window_tpye),
      window_size_(window_size),
      base_row_view_(base_table_schema_),
      aggr_row_view_(aggr_table_schema_),
      row_builder_(aggr_table_schema_) {
    for (int i = 0; i < base_meta.column_desc().size(); i++) {
        if (base_meta.column_desc(i).name() == aggr_col_) {
            aggr_col_idx_ = i;
            aggr_col_type_ = base_meta.column_desc(aggr_col_idx_).data_type();
        }
        if (base_meta.column_desc(i).name() == ts_col_) {
            ts_col_idx_ = i;
            ts_col_type_ = base_meta.column_desc(ts_col_idx_).data_type();
        }
    }
    // column name's existence will check in sql parse phase. it shouldn't occur here.
    if (aggr_col_idx_ == -1 && aggr_type_ != AggrType::kCount) {
        PDLOG(ERROR, "aggr_col not found in base table");
    }
    if (ts_col_idx_ == -1) {
        PDLOG(ERROR, "ts_col not found in base table");
    }
    auto dimension = dimensions_.Add();
    dimension->set_idx(0);
}

Aggregator::~Aggregator() {}

bool Aggregator::Update(const std::string& key, const std::string& row, const uint64_t& offset, bool recover) {
    if (!recover && GetStat() != AggrStat::kInited) {
        PDLOG(WARNING, "Aggregator status is not kInited");
        return false;
    }
    int8_t* row_ptr = reinterpret_cast<int8_t*>(const_cast<char*>(row.c_str()));
    int64_t cur_ts;
    switch (ts_col_type_) {
        case DataType::kBigInt: {
            base_row_view_.GetValue(row_ptr, ts_col_idx_, DataType::kBigInt, &cur_ts);
            break;
        }
        case DataType::kTimestamp: {
            base_row_view_.GetValue(row_ptr, ts_col_idx_, DataType::kTimestamp, &cur_ts);
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported timestamp data type");
            return false;
        }
    }
    std::string filter_key = "";
    if (filter_col_idx_ != -1) {
        if (!base_row_view_.IsNULL(row_ptr, filter_col_idx_)) {
            base_row_view_.GetStrValue(row_ptr, filter_col_idx_, &filter_key);
        }
    }

    if (!filter_key.empty() && window_type_ != WindowType::kRowsRange) {
        LOG(ERROR) << "unsupport rows bucket window for *_where agg op";
        return false;
    }

    AggrBufferLocked* aggr_buffer_lock;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = aggr_buffer_map_.find(key);
        if (it == aggr_buffer_map_.end()) {
            auto insert_pair = aggr_buffer_map_[key].emplace(filter_key, AggrBufferLocked{});
            aggr_buffer_lock = &insert_pair.first->second;
        } else {
            auto& filter_map = it->second;
            auto filter_it = filter_map.find(filter_key);
            if (filter_it == filter_map.end()) {
                auto insert_pair = filter_map.emplace(filter_key, AggrBufferLocked{});
                aggr_buffer_lock = &insert_pair.first->second;
            } else {
                aggr_buffer_lock = &filter_it->second;
            }
        }
    }

    std::unique_lock<std::mutex> lock(*aggr_buffer_lock->mu_);
    AggrBuffer& aggr_buffer = aggr_buffer_lock->buffer_;

    // init buffer timestamp range
    if (aggr_buffer.ts_begin_ == -1) {
        aggr_buffer.data_type_ = aggr_col_type_;
        aggr_buffer.ts_begin_ = AlignedStart(cur_ts);
        if (window_type_ == WindowType::kRowsRange) {
            aggr_buffer.ts_end_ = aggr_buffer.ts_begin_ + window_size_ - 1;
        }
    }

    if (offset < aggr_buffer.binlog_offset_) {
        if (recover) {
            return true;
        } else {
            PDLOG(ERROR, "logical error: current offset is smaller than binlog offset");
            return false;
        }
    }

    if (cur_ts < aggr_buffer.ts_begin_) {
        // handle the case that the current timestamp is smaller than the begin timestamp in aggregate buffer
        lock.unlock();
        if (recover) {
            // avoid out-of-order duplicate writes during the recovery phase
            return true;
        }
        bool ok = UpdateFlushedBuffer(key, filter_key, row_ptr, cur_ts, offset);
        if (!ok) {
            PDLOG(ERROR, "Update flushed buffer failed");
            return false;
        }
        return true;
    }

    if (CheckBufferFilled(cur_ts, aggr_buffer.ts_end_, aggr_buffer.aggr_cnt_)) {
        AggrBuffer flush_buffer = aggr_buffer;
        uint64_t latest_binlog = aggr_buffer.binlog_offset_ + 1;
        aggr_buffer.clear();
        aggr_buffer.binlog_offset_ = latest_binlog;
        aggr_buffer.ts_begin_ = AlignedStart(cur_ts);
        if (window_type_ == WindowType::kRowsRange) {
            aggr_buffer.ts_end_ = aggr_buffer.ts_begin_ + window_size_ - 1;
        }
        lock.unlock();
        FlushAggrBuffer(key, filter_key, flush_buffer);
        lock.lock();
    }

    aggr_buffer.aggr_cnt_++;
    aggr_buffer.binlog_offset_ = offset;
    if (window_type_ == WindowType::kRowsNum) {
        aggr_buffer.ts_end_ = cur_ts;
    }
    bool ok = UpdateAggrVal(base_row_view_, row_ptr, &aggr_buffer);
    if (!ok) {
        PDLOG(ERROR, "Update aggr value failed");
        return false;
    }
    return true;
}

bool Aggregator::FlushAll() {
    // TODO(nauta): optimize the flush process
    std::unique_lock<std::mutex> lock(mu_);
    std::unordered_map<std::string, std::unordered_map<std::string, AggrBuffer>> flushed_buffer_map;
    for (auto& it : aggr_buffer_map_) {
        for (auto& filter_it : it.second) {
            auto& aggr_buffer = filter_it.second.buffer_;
            if (aggr_buffer.aggr_cnt_ == 0) {
                continue;
            }
            flushed_buffer_map[it.first].emplace(filter_it.first, aggr_buffer);
        }
    }
    lock.unlock();
    for (auto& it : flushed_buffer_map) {
        for (auto& filter_it : it.second) {
            if (!FlushAggrBuffer(it.first, filter_it.first, filter_it.second)) {
                return false;
            }
        }
    }
    return true;
}

bool Aggregator::Init(std::shared_ptr<LogReplicator> base_replicator) {
    std::unique_lock<std::mutex> lock(mu_);
    if (GetStat() != AggrStat::kUnInit) {
        PDLOG(INFO, "aggregator status is %s", AggrStatToString(GetStat()));
        return true;
    }
    lock.unlock();
    if (!base_replicator) {
        return false;
    }
    status_.store(AggrStat::kRecovering, std::memory_order_relaxed);
    auto log_parts = base_replicator->GetLogPart();

    // TODO(nauta): support base table that existing data to init aggregator when deploy
    if (aggr_table_->GetRecordCnt() == 0 && log_parts->IsEmpty()) {
        status_.store(AggrStat::kInited, std::memory_order_relaxed);
        return true;
    }
    auto it = aggr_table_->NewTraverseIterator(0);
    it->SeekToFirst();
    uint64_t recovery_offset = UINT64_MAX;
    uint64_t aggr_latest_offset = 0;
    while (it->Valid()) {
        auto data_ptr = reinterpret_cast<const int8_t*>(it->GetValue().data());
        std::string pk, filter_key;
        aggr_row_view_.GetStrValue(data_ptr, 0, &pk);
        if (!aggr_row_view_.IsNULL(data_ptr, 6)) {
            aggr_row_view_.GetStrValue(data_ptr, 6, &filter_key);
        }
        auto insert_pair = aggr_buffer_map_[pk].insert(std::make_pair(filter_key, AggrBufferLocked{}));
        auto& buffer = insert_pair.first->second.buffer_;
        auto val = it->GetValue();
        int8_t* aggr_row_ptr = reinterpret_cast<int8_t*>(const_cast<char*>(val.data()));
        bool ok = GetAggrBufferFromRowView(aggr_row_view_, aggr_row_ptr, &buffer);
        if (!ok) {
            PDLOG(ERROR, "GetAggrBufferFromRowView failed");
            status_.store(AggrStat::kUnInit, std::memory_order_relaxed);
            return false;
        }
        recovery_offset = std::min(recovery_offset, buffer.binlog_offset_);
        aggr_latest_offset = std::max(aggr_latest_offset, buffer.binlog_offset_);
        int64_t latest_ts = buffer.ts_end_ + 1;
        uint64_t latest_binlog = buffer.binlog_offset_ + 1;
        buffer.clear();
        buffer.ts_begin_ = latest_ts;
        buffer.binlog_offset_ = latest_binlog;
        if (window_type_ == WindowType::kRowsRange) {
            buffer.ts_end_ = latest_ts + window_size_ - 1;
        }
        it->NextPK();
    }
    if (aggr_table_->GetRecordCnt() == 0) {
        recovery_offset = 0;
    }

    ::openmldb::log::LogReader log_reader(log_parts, base_replicator->GetLogPath(), false);
    log_reader.SetOffset(recovery_offset);
    ::openmldb::api::LogEntry entry;
    uint64_t cur_offset = recovery_offset;
    std::string buffer;
    int last_log_index = log_reader.GetLogIndex();
    while (true) {
        buffer.clear();
        ::openmldb::base::Slice record;
        ::openmldb::log::Status status = log_reader.ReadNextRecord(&record, &buffer);
        if (status.IsWaitRecord()) {
            int end_log_index = log_reader.GetEndLogIndex();
            int cur_log_index = log_reader.GetLogIndex();
            if (end_log_index >= 0 && end_log_index > cur_log_index) {
                log_reader.RollRLogFile();
                continue;
            }
            break;
        }
        if (status.IsEof()) {
            if (log_reader.GetLogIndex() != last_log_index) {
                last_log_index = log_reader.GetLogIndex();
                continue;
            }
            break;
        }

        if (!status.ok()) {
            PDLOG(WARNING, "read binlog failed: %s", status.ToString().c_str());
            continue;
        }

        bool ok = entry.ParseFromString(record.ToString());
        if (!ok) {
            PDLOG(WARNING, "parse binlog failed");
            continue;
        }
        if (cur_offset >= entry.log_index()) {
            continue;
        }

        // TODO(nauta): When the base table key is deleted, the pre-aggr table needs to be deleted at the same time.
        if (entry.has_method_type() && entry.method_type() == ::openmldb::api::MethodType::kDelete) {
            PDLOG(WARNING, "unsupport delete method for pre-aggr table");
            continue;
        }
        for (int i = 0; i < entry.dimensions_size(); i++) {
            const auto& dimension = entry.dimensions(i);
            if (dimension.idx() == index_pos_) {
                Update(dimension.key(), entry.value(), entry.log_index(), true);
                break;
            }
        }
        cur_offset = entry.log_index();
    }
    if (cur_offset < aggr_latest_offset) {
        PDLOG(ERROR, "base table is slower than aggregator");
        status_.store(AggrStat::kUnInit, std::memory_order_relaxed);
        return false;
    }
    PDLOG(INFO, "aggregator recovery finished");
    status_.store(AggrStat::kInited, std::memory_order_relaxed);
    return true;
}
bool Aggregator::GetAggrBuffer(const std::string& key, AggrBuffer** buffer) { return GetAggrBuffer(key, "", buffer); }

bool Aggregator::GetAggrBuffer(const std::string& key, const std::string& filter_key, AggrBuffer** buffer) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = aggr_buffer_map_.find(key);
    if (it == aggr_buffer_map_.end()) {
        return false;
    }
    *buffer = &aggr_buffer_map_[key][filter_key].buffer_;
    return true;
}

bool Aggregator::SetFilter(absl::string_view filter_col) {
    for (int i = 0; i < base_table_schema_.size(); i++) {
        if (base_table_schema_.Get(i).name() == filter_col) {
            filter_col_ = filter_col;
            filter_col_idx_ = i;
            return true;
        }
    }

    return false;
}

bool Aggregator::GetAggrBufferFromRowView(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* buffer) {
    if (buffer == nullptr) {
        return false;
    }
    buffer->data_type_ = aggr_col_type_;
    row_view.GetValue(row_ptr, 1, DataType::kTimestamp, &buffer->ts_begin_);
    row_view.GetValue(row_ptr, 2, DataType::kTimestamp, &buffer->ts_end_);
    row_view.GetValue(row_ptr, 3, DataType::kInt, &buffer->aggr_cnt_);
    row_view.GetValue(row_ptr, 5, DataType::kBigInt, &buffer->binlog_offset_);
    if (!DecodeAggrVal(row_ptr, buffer)) {
        return false;
    }
    return true;
}

bool Aggregator::FlushAggrBuffer(const std::string& key, const std::string& filter_key, const AggrBuffer& buffer) {
    std::string encoded_row;
    std::string aggr_val;
    if (!EncodeAggrVal(buffer, &aggr_val)) {
        PDLOG(ERROR, "Enocde aggr value to row failed");
        return false;
    }
    int str_length = key.size() + aggr_val.size() + filter_key.size();
    uint32_t row_size = row_builder_.CalTotalLength(str_length);
    encoded_row.resize(row_size);
    int8_t* row_ptr = reinterpret_cast<int8_t*>(&(encoded_row[0]));
    row_builder_.InitBuffer(row_ptr, row_size, true);
    row_builder_.SetString(row_ptr, row_size, 0, key.c_str(), key.size());
    row_builder_.SetTimestamp(row_ptr, 1, buffer.ts_begin_);
    row_builder_.SetTimestamp(row_ptr, 2, buffer.ts_end_);
    row_builder_.SetInt32(row_ptr, 3, buffer.aggr_cnt_);
    if ((aggr_type_ == AggrType::kMax || aggr_type_ == AggrType::kMin) && buffer.AggrValEmpty()) {
        row_builder_.SetNULL(row_ptr, row_size, 4);
    } else {
        row_builder_.SetString(row_ptr, row_size, 4, aggr_val.c_str(), aggr_val.size());
    }
    row_builder_.SetInt64(row_ptr, 5, buffer.binlog_offset_);
    if (!filter_key.empty()) {
        row_builder_.SetString(row_ptr, row_size, 6, filter_key.c_str(), filter_key.size());
    } else {
        row_builder_.SetNULL(row_ptr, row_size, 6);
    }

    int64_t time = ::baidu::common::timer::get_micros() / 1000;
    dimensions_.Mutable(0)->set_key(key);
    bool ok = aggr_table_->Put(time, encoded_row, dimensions_);
    if (!ok) {
        PDLOG(ERROR, "Aggregator put failed");
        return false;
    }
    ::openmldb::api::LogEntry entry;
    entry.set_pk(key);
    entry.set_ts(time);
    entry.set_value(encoded_row);
    entry.set_term(aggr_replicator_->GetLeaderTerm());
    entry.mutable_dimensions()->CopyFrom(dimensions_);
    aggr_replicator_->AppendEntry(entry);
    if (FLAGS_binlog_notify_on_put) {
        aggr_replicator_->Notify();
    }
    return true;
}

bool Aggregator::UpdateFlushedBuffer(const std::string& key, const std::string& filter_key, const int8_t* base_row_ptr,
                                     int64_t cur_ts, uint64_t offset) {
    auto it = aggr_table_->NewTraverseIterator(0);
    // If there is no repetition of ts, `seek` will locate to the position that less than ts.
    it->Seek(key, cur_ts + 1);
    AggrBuffer tmp_buffer;
    while (it->Valid()) {
        auto val = it->GetValue();
        int8_t* aggr_row_ptr = reinterpret_cast<int8_t*>(const_cast<char*>(val.data()));

        std::string pk;
        aggr_row_view_.GetStrValue(aggr_row_ptr, 0, &pk);
        // if pk doesn't match, break out
        if (key.compare(pk) != 0) {
            break;
        }

        int64_t ts_begin, ts_end;
        aggr_row_view_.GetValue(aggr_row_ptr, 1, DataType::kTimestamp, &ts_begin);
        aggr_row_view_.GetValue(aggr_row_ptr, 2, DataType::kTimestamp, &ts_end);
        // iterate further will never get the required aggr result
        if (cur_ts > ts_end) {
            break;
        }

        // ts == cur_ts + 1 may have duplicate entries
        if (cur_ts < ts_begin) {
            it->Next();
            continue;
        }

        std::string fk;
        if (!aggr_row_view_.IsNULL(aggr_row_ptr, 6)) {
            aggr_row_view_.GetStrValue(aggr_row_ptr, 6, &fk);
        }
        // filter_key doesn't match, continue
        if (filter_key.compare(fk) != 0) {
            it->Next();
            continue;
        }

        bool ok = GetAggrBufferFromRowView(aggr_row_view_, aggr_row_ptr, &tmp_buffer);
        if (!ok) {
            PDLOG(ERROR, "GetAggrBufferFromRowView failed");
            return false;
        }
        tmp_buffer.aggr_cnt_ += 1;
        tmp_buffer.binlog_offset_ = offset;
        break;
    }

    if (!tmp_buffer.IsInited()) {
        tmp_buffer.ts_begin_ = AlignedStart(cur_ts);
        if (window_type_ == WindowType::kRowsRange) {
            tmp_buffer.ts_end_ = tmp_buffer.ts_begin_ + window_size_ - 1;
        } else {
            tmp_buffer.ts_end_ = cur_ts;
        }
        tmp_buffer.aggr_cnt_ = 1;
        tmp_buffer.binlog_offset_ = offset;
    }
    bool ok = UpdateAggrVal(base_row_view_, base_row_ptr, &tmp_buffer);
    if (!ok) {
        PDLOG(ERROR, "UpdateAggrVal failed");
        return false;
    }

    ok = FlushAggrBuffer(key, filter_key, tmp_buffer);
    if (!ok) {
        PDLOG(ERROR, "FlushAggrBuffer failed");
        return false;
    }
    return true;
}

bool Aggregator::CheckBufferFilled(int64_t cur_ts, int64_t buffer_end, int32_t buffer_cnt) {
    if (window_type_ == WindowType::kRowsRange && cur_ts > buffer_end) {
        return true;
    } else if (window_type_ == WindowType::kRowsNum && buffer_cnt >= window_size_) {
        return true;
    }
    return false;
}

SumAggregator::SumAggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                             std::shared_ptr<Table> aggr_table, std::shared_ptr<LogReplicator> aggr_replicator,
                             const uint32_t& index_pos, const std::string& aggr_col, const AggrType& aggr_type,
                             const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : Aggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col, window_tpye,
                 window_size) {}

bool SumAggregator::UpdateAggrVal(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    if (row_view.IsNULL(row_ptr, aggr_col_idx_)) {
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kInt: {
            int32_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kFloat: {
            float val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vfloat += val;
            break;
        }
        case DataType::kDouble: {
            double val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    aggr_buffer->non_null_cnt_++;
    return true;
}

bool SumAggregator::EncodeAggrVal(const AggrBuffer& buffer, std::string* aggr_val) {
    switch (aggr_col_type_) {
        case DataType::kSmallInt:
        case DataType::kInt:
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t tmp_val = buffer.aggr_val_.vlong;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(int64_t));
            break;
        }
        case DataType::kFloat: {
            float tmp_val = buffer.aggr_val_.vfloat;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(float));
            break;
        }
        case DataType::kDouble: {
            double tmp_val = buffer.aggr_val_.vdouble;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(double));
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    return true;
}

bool SumAggregator::DecodeAggrVal(const int8_t* row_ptr, AggrBuffer* buffer) {
    char* aggr_val = NULL;
    uint32_t ch_length = 0;
    if (aggr_row_view_.GetValue(row_ptr, 4, &aggr_val, &ch_length) == 1) {
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt:
        case DataType::kInt:
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t origin_val = *reinterpret_cast<int64_t*>(aggr_val);
            buffer->aggr_val_.vlong = origin_val;
            break;
        }
        case DataType::kFloat: {
            float origin_val = *reinterpret_cast<float*>(aggr_val);
            buffer->aggr_val_.vfloat = origin_val;
            break;
        }
        case DataType::kDouble: {
            double origin_val = *reinterpret_cast<double*>(aggr_val);
            buffer->aggr_val_.vdouble = origin_val;
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    return true;
}

MinMaxBaseAggregator::MinMaxBaseAggregator(const ::openmldb::api::TableMeta& base_meta,
                                           const ::openmldb::api::TableMeta& aggr_meta,
                                           std::shared_ptr<Table> aggr_table,
                                           std::shared_ptr<LogReplicator> aggr_replicator, const uint32_t& index_pos,
                                           const std::string& aggr_col, const AggrType& aggr_type,
                                           const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : Aggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col, window_tpye,
                 window_size) {}

bool MinMaxBaseAggregator::EncodeAggrVal(const AggrBuffer& buffer, std::string* aggr_val) {
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t tmp_val = buffer.aggr_val_.vsmallint;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(int16_t));
            break;
        }
        case DataType::kDate:
        case DataType::kInt: {
            int32_t tmp_val = buffer.aggr_val_.vint;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(int32_t));
            break;
        }
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t tmp_val = buffer.aggr_val_.vlong;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(int64_t));
            break;
        }
        case DataType::kFloat: {
            float tmp_val = buffer.aggr_val_.vfloat;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(float));
            break;
        }
        case DataType::kDouble: {
            double tmp_val = buffer.aggr_val_.vdouble;
            aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(double));
            break;
        }
        case DataType::kString:
        case DataType::kVarchar: {
            aggr_val->assign(buffer.aggr_val_.vstring.data, buffer.aggr_val_.vstring.len);
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    return true;
}

bool MinMaxBaseAggregator::DecodeAggrVal(const int8_t* row_ptr, AggrBuffer* buffer) {
    char* aggr_val = NULL;
    uint32_t ch_length = 0;
    if (aggr_row_view_.GetValue(row_ptr, 4, &aggr_val, &ch_length) == 1) {  // null value
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t origin_val = *reinterpret_cast<int16_t*>(aggr_val);
            buffer->aggr_val_.vsmallint = origin_val;
            break;
        }
        case DataType::kDate:
        case DataType::kInt: {
            int32_t origin_val = *reinterpret_cast<int32_t*>(aggr_val);
            buffer->aggr_val_.vint = origin_val;
            break;
        }
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t origin_val = *reinterpret_cast<int64_t*>(aggr_val);
            buffer->aggr_val_.vlong = origin_val;
            break;
        }
        case DataType::kFloat: {
            float origin_val = *reinterpret_cast<float*>(aggr_val);
            buffer->aggr_val_.vfloat = origin_val;
            break;
        }
        case DataType::kDouble: {
            double origin_val = *reinterpret_cast<double*>(aggr_val);
            buffer->aggr_val_.vdouble = origin_val;
            break;
        }
        case DataType::kString:
        case DataType::kVarchar: {
            auto& vstr = buffer->aggr_val_.vstring;
            if (vstr.data != NULL && ch_length > vstr.len) {
                delete[] vstr.data;
                vstr.data = NULL;
            }
            if (vstr.data == NULL) {
                vstr.data = new char[ch_length];
            }
            vstr.len = ch_length;
            memcpy(vstr.data, aggr_val, ch_length);
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    return true;
}

MinAggregator::MinAggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                             std::shared_ptr<Table> aggr_table, std::shared_ptr<LogReplicator> aggr_replicator,
                             const uint32_t& index_pos, const std::string& aggr_col, const AggrType& aggr_type,
                             const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : MinMaxBaseAggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col,
                           window_tpye, window_size) {}

bool MinAggregator::UpdateAggrVal(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    if (row_view.IsNULL(row_ptr, aggr_col_idx_)) {
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val < aggr_buffer->aggr_val_.vsmallint) {
                aggr_buffer->aggr_val_.vsmallint = val;
            }
            break;
        }
        case DataType::kDate:
        case DataType::kInt: {
            int32_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val < aggr_buffer->aggr_val_.vint) {
                aggr_buffer->aggr_val_.vint = val;
            }
            break;
        }
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val < aggr_buffer->aggr_val_.vlong) {
                aggr_buffer->aggr_val_.vlong = val;
            }
            break;
        }
        case DataType::kFloat: {
            float val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val < aggr_buffer->aggr_val_.vfloat) {
                aggr_buffer->aggr_val_.vfloat = val;
            }
            break;
        }
        case DataType::kDouble: {
            double val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val < aggr_buffer->aggr_val_.vdouble) {
                aggr_buffer->aggr_val_.vdouble = val;
            }
            break;
        }
        case DataType::kString:
        case DataType::kVarchar: {
            char* ch = NULL;
            uint32_t ch_length = 0;
            row_view.GetValue(row_ptr, aggr_col_idx_, &ch, &ch_length);
            auto& aggr_val = aggr_buffer->aggr_val_.vstring;
            if (aggr_buffer->AggrValEmpty() || StringCompare(ch, ch_length, aggr_val.data, aggr_val.len) < 0) {
                if (aggr_val.data != NULL && ch_length > aggr_val.len) {
                    delete[] aggr_val.data;
                    aggr_val.data = NULL;
                }
                if (aggr_val.data == NULL) {
                    aggr_val.data = new char[ch_length];
                }
                aggr_val.len = ch_length;
                memcpy(aggr_val.data, ch, ch_length);
            }
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    aggr_buffer->non_null_cnt_++;
    return true;
}

MaxAggregator::MaxAggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                             std::shared_ptr<Table> aggr_table, std::shared_ptr<LogReplicator> aggr_replicator,
                             const uint32_t& index_pos, const std::string& aggr_col, const AggrType& aggr_type,
                             const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : MinMaxBaseAggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col,
                           window_tpye, window_size) {}

bool MaxAggregator::UpdateAggrVal(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    if (row_view.IsNULL(row_ptr, aggr_col_idx_)) {
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val > aggr_buffer->aggr_val_.vsmallint) {
                aggr_buffer->aggr_val_.vsmallint = val;
            }
            break;
        }
        case DataType::kDate:
        case DataType::kInt: {
            int32_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val > aggr_buffer->aggr_val_.vint) {
                aggr_buffer->aggr_val_.vint = val;
            }
            break;
        }
        case DataType::kTimestamp:
        case DataType::kBigInt: {
            int64_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val > aggr_buffer->aggr_val_.vlong) {
                aggr_buffer->aggr_val_.vlong = val;
            }
            break;
        }
        case DataType::kFloat: {
            float val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val > aggr_buffer->aggr_val_.vfloat) {
                aggr_buffer->aggr_val_.vfloat = val;
            }
            break;
        }
        case DataType::kDouble: {
            double val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            if (aggr_buffer->AggrValEmpty() || val > aggr_buffer->aggr_val_.vdouble) {
                aggr_buffer->aggr_val_.vdouble = val;
            }
            break;
        }
        case DataType::kString:
        case DataType::kVarchar: {
            char* ch = NULL;
            uint32_t ch_length = 0;
            row_view.GetValue(row_ptr, aggr_col_idx_, &ch, &ch_length);
            auto& aggr_val = aggr_buffer->aggr_val_.vstring;
            if (aggr_buffer->AggrValEmpty() || StringCompare(ch, ch_length, aggr_val.data, aggr_val.len) > 0) {
                if (aggr_val.data != NULL && ch_length > aggr_val.len) {
                    delete[] aggr_val.data;
                    aggr_val.data = NULL;
                }
                if (aggr_val.data == NULL) {
                    aggr_val.data = new char[ch_length];
                }
                aggr_val.len = ch_length;
                memcpy(aggr_val.data, ch, ch_length);
            }
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    aggr_buffer->non_null_cnt_++;
    return true;
}

CountAggregator::CountAggregator(const ::openmldb::api::TableMeta& base_meta,
                                 const ::openmldb::api::TableMeta& aggr_meta, std::shared_ptr<Table> aggr_table,
                                 std::shared_ptr<LogReplicator> aggr_replicator, const uint32_t& index_pos,
                                 const std::string& aggr_col, const AggrType& aggr_type, const std::string& ts_col,
                                 WindowType window_tpye, uint32_t window_size)
    : Aggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col, window_tpye,
                 window_size) {
    if (aggr_col == "*") {
        count_all = true;
    }
}

bool CountAggregator::EncodeAggrVal(const AggrBuffer& buffer, std::string* aggr_val) {
    int64_t tmp_val = buffer.non_null_cnt_;
    aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(int64_t));
    return true;
}

bool CountAggregator::DecodeAggrVal(const int8_t* row_ptr, AggrBuffer* buffer) {
    char* aggr_val = NULL;
    uint32_t ch_length = 0;
    if (aggr_row_view_.GetValue(row_ptr, 4, &aggr_val, &ch_length) == 1) {
        return true;
    }
    buffer->non_null_cnt_ = *reinterpret_cast<int64_t*>(aggr_val);
    return true;
}

bool CountAggregator::UpdateAggrVal(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    if (count_all || !row_view.IsNULL(row_ptr, aggr_col_idx_)) {
        aggr_buffer->non_null_cnt_++;
    }
    return true;
}

AvgAggregator::AvgAggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                             std::shared_ptr<Table> aggr_table, std::shared_ptr<LogReplicator> aggr_replicator,
                             const uint32_t& index_pos, const std::string& aggr_col, const AggrType& aggr_type,
                             const std::string& ts_col, WindowType window_tpye, uint32_t window_size)
    : Aggregator(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col, aggr_type, ts_col, window_tpye,
                 window_size) {}

bool AvgAggregator::UpdateAggrVal(const codec::RowView& row_view, const int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    if (row_view.IsNULL(row_ptr, aggr_col_idx_)) {
        return true;
    }
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        case DataType::kInt: {
            int32_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        case DataType::kBigInt: {
            int64_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        case DataType::kFloat: {
            float val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        case DataType::kDouble: {
            double val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        default: {
            PDLOG(ERROR, "Unsupported data type");
            return false;
        }
    }
    aggr_buffer->non_null_cnt_++;
    return true;
}

bool AvgAggregator::EncodeAggrVal(const AggrBuffer& buffer, std::string* aggr_val) {
    double tmp_val = buffer.aggr_val_.vdouble;
    aggr_val->assign(reinterpret_cast<char*>(&tmp_val), sizeof(double));
    aggr_val->append(reinterpret_cast<char*>(const_cast<int64_t*>(&buffer.non_null_cnt_)), sizeof(int64_t));
    return true;
}

bool AvgAggregator::DecodeAggrVal(const int8_t* row_ptr, AggrBuffer* buffer) {
    char* aggr_val = NULL;
    uint32_t ch_length = 0;
    if (aggr_row_view_.GetValue(row_ptr, 4, &aggr_val, &ch_length) == 1) {
        return true;
    }
    double origin_val = *reinterpret_cast<double*>(aggr_val);
    buffer->aggr_val_.vdouble = origin_val;
    buffer->non_null_cnt_ = *reinterpret_cast<int64_t*>(aggr_val + sizeof(double));
    return true;
}

std::shared_ptr<Aggregator> CreateAggregator(const ::openmldb::api::TableMeta& base_meta,
                                             const ::openmldb::api::TableMeta& aggr_meta,
                                             std::shared_ptr<Table> aggr_table,
                                             std::shared_ptr<LogReplicator> aggr_replicator, const uint32_t& index_pos,
                                             const std::string& aggr_col, const std::string& aggr_func,
                                             const std::string& ts_col, const std::string& bucket_size,
                                             const std::string& filter_col) {
    std::string aggr_type = boost::to_lower_copy(aggr_func);
    WindowType window_type;
    uint32_t window_size;
    if (::openmldb::base::IsNumber(bucket_size)) {
        window_type = WindowType::kRowsNum;
        window_size = std::stoi(bucket_size);
    } else {
        window_type = WindowType::kRowsRange;
        if (bucket_size.empty()) {
            PDLOG(ERROR, "Bucket size is empty");
            return {};
        }
        char time_unit = tolower(bucket_size.back());
        std::string time_size = bucket_size.substr(0, bucket_size.size() - 1);
        boost::trim(time_size);
        if (!::openmldb::base::IsNumber(time_size)) {
            PDLOG(ERROR, "Bucket size is not a number");
            return {};
        }
        switch (time_unit) {
            case 's':
                window_size = std::stoi(time_size) * 1000;
                break;
            case 'm':
                window_size = std::stoi(time_size) * 1000 * 60;
                break;
            case 'h':
                window_size = std::stoi(time_size) * 1000 * 60 * 60;
                break;
            case 'd':
                window_size = std::stoi(time_size) * 1000 * 60 * 60 * 24;
                break;
            default: {
                PDLOG(ERROR, "Unsupported time unit");
                return {};
            }
        }
    }

    std::shared_ptr<Aggregator> agg;
    if (aggr_type == "sum" || aggr_type == "sum_where") {
        agg = std::make_shared<SumAggregator>(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col,
                                               AggrType::kSum, ts_col, window_type, window_size);
    } else if (aggr_type == "min" || aggr_type == "min_where") {
        agg = std::make_shared<MinAggregator>(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col,
                                               AggrType::kMin, ts_col, window_type, window_size);
    } else if (aggr_type == "max" || aggr_type == "max_where") {
        agg = std::make_shared<MaxAggregator>(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col,
                                              AggrType::kMax, ts_col, window_type, window_size);
    } else if (aggr_type == "count" || aggr_type == "count_where") {
        agg = std::make_shared<CountAggregator>(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col,
                                                AggrType::kCount, ts_col, window_type, window_size);
    } else if (aggr_type == "avg" || aggr_type == "avg_where") {
        agg = std::make_shared<AvgAggregator>(base_meta, aggr_meta, aggr_table, aggr_replicator, index_pos, aggr_col,
                                              AggrType::kAvg, ts_col, window_type, window_size);
    } else {
        PDLOG(ERROR, "Unsupported aggregate function type");
        return {};
    }

    if (filter_col.empty() || !absl::EndsWithIgnoreCase(aggr_type, "_where")) {
        // min/max/count/avg/sum ops
        return agg;
    }

    // _where variant
    if (filter_col.empty()) {
        PDLOG(ERROR, "no filter column specified for %s", aggr_type);
        return {};
    }
    if (!agg->SetFilter(filter_col)) {
        PDLOG(ERROR, "can not find filter column '%s' for %s", filter_col, aggr_type);
        return {};
    }
    return agg;
}

}  // namespace storage
}  // namespace openmldb
