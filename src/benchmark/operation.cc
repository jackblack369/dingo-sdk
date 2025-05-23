// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "benchmark/operation.h"

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "benchmark/dataset.h"
#include "common/logging.h"
#include "dingosdk/vector.h"
#include "fmt/core.h"
#include "gflags/gflags.h"
#include "gflags/gflags_declare.h"
#include "glog/logging.h"
#include "util.h"

DECLARE_uint32(diskann_search_beamwidth);
DECLARE_string(vector_index_type);
DEFINE_string(benchmark, "fillseq", "Benchmark type");
DEFINE_validator(benchmark, [](const char*, const std::string& value) -> bool {
  return dingodb::benchmark::IsSupportBenchmarkType(value) || value == "preprocess";
});

DEFINE_uint32(key_size, 64, "Key size");
DEFINE_uint32(value_size, 256, "Value size");

DEFINE_uint32(batch_size, 1, "Batch size");

DEFINE_uint32(arrange_kv_num, 10000, "The number of kv for read");

DEFINE_bool(is_pessimistic_txn, false, "Optimistic or pessimistic transaction");
DEFINE_string(txn_isolation_level, "SI", "Transaction isolation level");
DEFINE_validator(txn_isolation_level, [](const char*, const std::string& value) -> bool {
  auto isolation_level = dingodb::benchmark::ToUpper(value);
  return isolation_level == "SI" || isolation_level == "RC";
});

// vector search
DECLARE_string(vector_dataset);
DECLARE_uint32(vector_search_topk);
DECLARE_bool(with_vector_data);
DECLARE_bool(with_scalar_data);
DECLARE_bool(with_table_data);
DECLARE_bool(vector_search_use_brute_force);
DECLARE_bool(vector_search_enable_range_search);
DECLARE_double(vector_search_radius);
DEFINE_string(vector_search_filter_type, "", "Vector search filter type, e.g. pre/post");
DEFINE_validator(vector_search_filter_type, [](const char*, const std::string& value) -> bool {
  auto filter_type = dingodb::benchmark::ToUpper(value);
  return filter_type.empty() || filter_type == "PRE" || filter_type == "POST";
});

DEFINE_string(vector_search_filter_source, "", "Vector search filter source, e.g. scalar/table/vector_id");
DEFINE_validator(vector_search_filter_source, [](const char*, const std::string& value) -> bool {
  auto filter_source = dingodb::benchmark::ToUpper(value);
  return filter_source.empty() || filter_source == "SCALAR" || filter_source == "TABLE" || filter_source == "VECTOR_ID";
});

DEFINE_uint32(vector_search_nprobe, 80, "Vector search nprobe param");
DEFINE_uint32(vector_search_ef, 128, "Vector search ef param");

DECLARE_uint32(vector_dimension);
DECLARE_string(vector_value_type);

DEFINE_uint32(vector_put_batch_size, 512, "Vector put batch size");

DEFINE_uint32(vector_arrange_concurrency, 10, "Vector arrange put concurrency");

DEFINE_bool(vector_search_arrange_data, true, "Arrange data for search");

DEFINE_bool(use_coprocessor, false, "Vector search use coprocessor");

DEFINE_uint32(vector_search_filter_vector_id_num, 10000, "Vector search filter vector id num");
DEFINE_bool(filter_vector_id_is_negation, false, "Use negation vector id filter");

namespace dingodb {
namespace benchmark {

static const std::string kClientRaw = "w";
static const std::string kClientTxn = "x";

static const char kAlphabet[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
                                 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
                                 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

using BuildFuncer = std::function<OperationPtr(std::shared_ptr<sdk::Client> client)>;
using OperationBuilderMap = std::map<std::string, BuildFuncer>;

static OperationBuilderMap support_operations = {
    {"fillseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillSeqOperation>(client); }},
    {"fillrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillRandomOperation>(client); }},
    {"readseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<ReadSeqOperation>(client); }},
    {"readrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<ReadRandomOperation>(client); }},
    {"readmissing",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<ReadMissingOperation>(client);
     }},
    {"filltxnseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<FillTxnSeqOperation>(client); }},
    {"filltxnrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<FillTxnRandomOperation>(client);
     }},
    {"readtxnseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr { return std::make_shared<TxnReadSeqOperation>(client); }},
    {"readtxnrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<TxnReadRandomOperation>(client);
     }},
    {"readtxnmissing",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<TxnReadMissingOperation>(client);
     }},
    {"fillvectorseq",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<VectorFillSeqOperation>(client);
     }},
    {"fillvectorrandom",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<VectorFillRandomOperation>(client);
     }},
    {"searchvector",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<VectorSearchOperation>(client);
     }},
    {"queryvector",
     [](std::shared_ptr<sdk::Client> client) -> OperationPtr {
       return std::make_shared<VectorQueryOperation>(client);
     }},
};

static sdk::TransactionIsolation GetTxnIsolationLevel() {
  auto isolation_level = dingodb::benchmark::ToUpper(FLAGS_txn_isolation_level);
  if (isolation_level == "SI") {
    return sdk::TransactionIsolation::kSnapshotIsolation;
  } else if (isolation_level == "RC") {
    return sdk::TransactionIsolation::kReadCommitted;
  }

  LOG(FATAL) << fmt::format("Not support transaction isolation level: {}", FLAGS_txn_isolation_level);
  return sdk::TransactionIsolation::kReadCommitted;
}

// rand string
static std::string GenRandomString(int len) {
  std::string result;
  int alphabet_len = sizeof(kAlphabet);

  std::mt19937 rng;
  rng.seed(std::random_device()());
  std::uniform_int_distribution<std::mt19937::result_type> distrib(1, 1000000000);
  for (int i = 0; i < len; ++i) {
    result.append(1, kAlphabet[distrib(rng) % alphabet_len]);
  }

  return result;
}

static std::string GenSeqString(int num, int len) { return fmt::format("{0:0{1}}", num, len); }

static std::string EncodeRawKey(const std::string& str) { return kClientRaw + str; }
static std::string EncodeTxnKey(const std::string& str) { return kClientTxn + str; }

BaseOperation::BaseOperation(std::shared_ptr<sdk::Client> client) : client(client) {
  sdk::RawKV* tmp;
  auto status = client->NewRawKV(&tmp);
  if (!status.IsOK()) {
    LOG(FATAL) << fmt::format("New RawKv failed, error: {}", status.ToString());
  }
  raw_kv.reset(tmp);
}

Operation::Result BaseOperation::KvPut(RegionEntryPtr region_entry, bool is_random) {
  Operation::Result result;
  std::string& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;

  int random_str_len = FLAGS_key_size - prefix.size();

  size_t count = counter.fetch_add(1, std::memory_order_relaxed);
  std::string key;
  if (is_random) {
    key = EncodeRawKey(prefix + GenRandomString(random_str_len));
  } else {
    key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
  }

  std::string value = GenRandomString(FLAGS_value_size);
  result.write_bytes = key.size() + value.size();

  int64_t start_time = dingodb::benchmark::TimestampUs();

  result.status = raw_kv->Put(key, value);

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvBatchPut(RegionEntryPtr region_entry, bool is_random) {
  Operation::Result result;
  std::string& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;

  int random_str_len = FLAGS_key_size - prefix.size();

  std::vector<sdk::KVPair> kvs;
  for (int i = 0; i < FLAGS_batch_size; ++i) {
    sdk::KVPair kv;
    if (is_random) {
      kv.key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    } else {
      size_t count = counter.fetch_add(1, std::memory_order_relaxed);
      kv.key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
    }

    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = dingodb::benchmark::TimestampUs();

  result.status = raw_kv->BatchPut(kvs);

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvGet(std::string key) {
  Operation::Result result;

  int64_t start_time = dingodb::benchmark::TimestampUs();

  std::string value;
  result.status = raw_kv->Get(key, value);

  result.read_bytes += value.size();

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvBatchGet(const std::vector<std::string>& keys) {
  Operation::Result result;

  int64_t start_time = dingodb::benchmark::TimestampUs();

  std::vector<sdk::KVPair> kvs;
  result.status = raw_kv->BatchGet(keys, kvs);

  for (auto& kv : kvs) {
    result.read_bytes += kv.key.size() + kv.value.size();
  }

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  return result;
}

Operation::Result BaseOperation::KvTxnPut(std::vector<RegionEntryPtr>& region_entries, bool is_random) {
  std::vector<sdk::KVPair> kvs;
  for (const auto& region_entry : region_entries) {
    int random_str_len = FLAGS_key_size - region_entry->prefix.size();

    sdk::KVPair kv;
    size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = is_random ? EncodeTxnKey(region_entry->prefix + GenRandomString(random_str_len))
                       : EncodeTxnKey(region_entry->prefix + GenSeqString(count, random_str_len));

    kv.value = GenRandomString(FLAGS_value_size);
    kvs.push_back(kv);
  }

  return KvTxnPut(kvs);
}

Operation::Result BaseOperation::KvTxnPut(const std::vector<sdk::KVPair>& kvs) {
  Operation::Result result;

  for (const auto& kv : kvs) {
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("new transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  for (const auto& kv : kvs) {
    result.status = txn->Put(kv.key, kv.value);
    if (!result.status.IsOK()) {
      LOG(ERROR) << fmt::format("transaction put failed, error: {}", result.status.ToString());
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("pre commit transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->Commit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("commit transaction failed, error: {}", result.status.ToString());
  }

end:
  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnBatchPut(std::vector<RegionEntryPtr>& region_entries, bool is_random) {
  std::vector<sdk::KVPair> kvs;
  for (const auto& region_entry : region_entries) {
    int random_str_len = FLAGS_key_size - region_entry->prefix.size();

    for (int i = 0; i < FLAGS_batch_size; ++i) {
      sdk::KVPair kv;
      size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
      kv.key = is_random ? EncodeTxnKey(region_entry->prefix + GenRandomString(random_str_len))
                         : EncodeTxnKey(region_entry->prefix + GenSeqString(count, random_str_len));
      kv.value = GenRandomString(FLAGS_value_size);

      kvs.push_back(kv);
    }
  }

  return KvTxnBatchPut(kvs);
}

Operation::Result BaseOperation::KvTxnBatchPut(const std::vector<sdk::KVPair>& kvs) {
  Operation::Result result;

  for (const auto& kv : kvs) {
    result.write_bytes += kv.key.size() + kv.value.size();
  }

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("new transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->BatchPut(kvs);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("transaction batch put failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("pre commit transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->Commit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("commit transaction failed, error: {}", result.status.ToString());
  }

end:
  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnGet(const std::vector<std::string>& keys) {
  Operation::Result result;

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("new transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  for (const auto& key : keys) {
    std::string value;
    result.status = txn->Get(key, value);
    if (!result.status.IsOK()) {
      LOG(ERROR) << fmt::format("transaction get failed, error: {}", result.status.ToString());
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("pre commit transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->Commit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("commit transaction failed, error: {}", result.status.ToString());
  }

end:
  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;
  delete txn;

  return result;
}

Operation::Result BaseOperation::KvTxnBatchGet(const std::vector<std::vector<std::string>>& keys) {
  Operation::Result result;

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::Transaction* txn = nullptr;
  sdk::TransactionOptions options;
  options.kind = FLAGS_is_pessimistic_txn ? sdk::TransactionKind::kPessimistic : sdk::TransactionKind::kOptimistic;
  options.isolation = GetTxnIsolationLevel();

  result.status = client->NewTransaction(options, &txn);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("new transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  for (const auto& batch_keys : keys) {
    std::vector<sdk::KVPair> kvs;
    result.status = txn->BatchGet(batch_keys, kvs);
    if (!result.status.IsOK()) {
      LOG(ERROR) << fmt::format("transaction batch get failed, error: {}", result.status.ToString());
      goto end;
    }
  }

  result.status = txn->PreCommit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("pre commit transaction failed, error: {}", result.status.ToString());
    goto end;
  }

  result.status = txn->Commit();
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("commit transaction failed, error: {}", result.status.ToString());
  }

end:
  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;
  delete txn;

  return result;
}

static bool IsMonotoneIncreasing(const std::vector<sdk::VectorWithId>& vector_with_ids) {
  int64_t vector_id = 0;
  for (const auto& vector_with_id : vector_with_ids) {
    if (vector_with_id.id <= vector_id) {
      LOG(INFO) << fmt::format("is not monotoe increasing: {} {}", vector_id, vector_with_id.id);
      return false;
    }
    vector_id = vector_with_id.id;
  }

  return true;
}

Operation::Result BaseOperation::VectorPut(VectorIndexEntryPtr entry, std::vector<sdk::VectorWithId>& vector_with_ids) {
  Operation::Result result;

  IsMonotoneIncreasing(vector_with_ids);

  for (const auto& vector_with_id : vector_with_ids) {
    result.write_bytes += vector_with_id.vector.Size();
  }

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::VectorClient* vector_client = nullptr;
  result.status = client->NewVectorClient(&vector_client);
  if (!result.status.IsOK()) {
    return result;
  }

  if (FLAGS_vector_index_type != "DISKANN") {
    result.status = vector_client->AddByIndexId(entry->index_id, vector_with_ids);
  } else {
    result.status = vector_client->ImportAddByIndexId(entry->index_id, vector_with_ids);
  }

  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("put vector failed, error: {}", result.status.ToString());
  }

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  delete vector_client;

  return result;
}

Operation::Result BaseOperation::VectorSearch(VectorIndexEntryPtr entry,
                                              const std::vector<sdk::VectorWithId>& vector_with_ids,
                                              const sdk::SearchParam& search_param) {
  Operation::Result result;

  for (const auto& vector_with_id : vector_with_ids) {
    result.write_bytes += vector_with_id.vector.Size();
  }
  result.write_bytes += search_param.vector_ids.size() * sizeof(int64_t);

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::VectorClient* vector_client = nullptr;
  result.status = client->NewVectorClient(&vector_client);
  if (!result.status.IsOK()) {
    return result;
  }

  result.status =
      vector_client->SearchByIndexId(entry->index_id, search_param, vector_with_ids, result.vector_search_results);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("search vector failed, error: {}", result.status.ToString());
  }

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  delete vector_client;

  LOG(INFO) << fmt::format("result size: {} {}", result.vector_search_results.size(),
                           result.vector_search_results[0].vector_datas.size());

  return result;
}

Operation::Result BaseOperation::VectorBatchQuery(VectorIndexEntryPtr entry, const sdk::QueryParam& query_param) {
  Operation::Result result;

  result.write_bytes = query_param.vector_ids.size() * sizeof(int64_t);

  int64_t start_time = dingodb::benchmark::TimestampUs();

  sdk::VectorClient* vector_client = nullptr;
  result.status = client->NewVectorClient(&vector_client);
  if (!result.status.IsOK()) {
    return result;
  }

  result.status = vector_client->BatchQueryByIndexId(entry->index_id, query_param, result.vector_query_result);
  if (!result.status.IsOK()) {
    LOG(ERROR) << fmt::format("query vector failed, error: {}", result.status.ToString());
  }

  result.eplased_time = dingodb::benchmark::TimestampUs() - start_time;

  delete vector_client;

  return result;
}

Operation::Result FillSeqOperation::Execute(RegionEntryPtr region_entry) {
  return FLAGS_batch_size == 1 ? KvPut(region_entry, false) : KvBatchPut(region_entry, false);
}

Operation::Result FillRandomOperation::Execute(RegionEntryPtr region_entry) {
  return FLAGS_batch_size == 1 ? KvPut(region_entry, true) : KvBatchPut(region_entry, true);
}

bool ReadOperation::Arrange(RegionEntryPtr region_entry) {
  std::string& prefix = region_entry->prefix;
  int random_str_len = FLAGS_key_size - prefix.size();

  uint32_t batch_size = 256;
  std::vector<sdk::KVPair> kvs;
  kvs.reserve(batch_size);
  for (uint32_t i = 0; i < FLAGS_arrange_kv_num; ++i) {
    sdk::KVPair kv;
    size_t count = region_entry->counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = EncodeRawKey(prefix + GenSeqString(count, random_str_len));
    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    region_entry->keys.push_back(kv.key);

    if ((i + 1) % batch_size == 0 || (i + 1 == FLAGS_arrange_kv_num)) {
      auto status = raw_kv->BatchPut(kvs);
      if (!status.ok()) {
        return false;
      }
      kvs.clear();
      std::cout << '\r'
                << fmt::format("region({}) put data({}) progress [{}%]", FLAGS_arrange_kv_num, prefix,
                               i * 100 / FLAGS_arrange_kv_num)
                << std::flush;
    }
  }

  std::cout << "\r" << fmt::format("region({}) put data({}) ............ done", prefix, FLAGS_arrange_kv_num) << '\n';

  return true;
}

Operation::Result ReadSeqOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    return KvGet(keys[region_entry->read_index++ % keys.size()]);
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      batch_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }
    return KvBatchGet(batch_keys);
  }
}

Operation::Result ReadRandomOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
    return KvGet(keys[index]);
  } else {
    std::vector<std::string> keys;
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      keys.push_back(keys[index]);
    }
    return KvBatchGet(keys);
  }
}

Operation::Result ReadMissingOperation::Execute(RegionEntryPtr region_entry) {
  std::string& prefix = region_entry->prefix;

  int random_str_len = FLAGS_key_size + 4 - prefix.size();
  if (FLAGS_batch_size <= 1) {
    std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    return KvGet(key);
  } else {
    std::vector<std::string> keys;
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      keys.push_back(key);
    }
    return KvBatchGet(keys);
  }
}

Operation::Result FillTxnSeqOperation::Execute(RegionEntryPtr region_entry) {
  std::vector<RegionEntryPtr> region_entries = {region_entry};
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, false) : KvTxnBatchPut(region_entries, false);
}

Operation::Result FillTxnSeqOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, false) : KvTxnBatchPut(region_entries, false);
}

Operation::Result FillTxnRandomOperation::Execute(RegionEntryPtr region_entry) {
  std::vector<RegionEntryPtr> region_entries = {region_entry};
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, true) : KvTxnBatchPut(region_entries, true);
}

Operation::Result FillTxnRandomOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  return FLAGS_batch_size == 1 ? KvTxnPut(region_entries, true) : KvTxnBatchPut(region_entries, true);
}

bool TxnReadOperation::Arrange(RegionEntryPtr region_entry) {
  auto& prefix = region_entry->prefix;
  auto& counter = region_entry->counter;
  auto& keys = region_entry->keys;

  int random_str_len = FLAGS_key_size - prefix.size();

  uint32_t batch_size = 256;
  std::vector<sdk::KVPair> kvs;
  kvs.reserve(batch_size);
  for (uint32_t i = 0; i < FLAGS_arrange_kv_num; ++i) {
    sdk::KVPair kv;
    size_t count = counter.fetch_add(1, std::memory_order_relaxed);
    kv.key = EncodeTxnKey(prefix + GenSeqString(count, random_str_len));
    kv.value = GenRandomString(FLAGS_value_size);

    kvs.push_back(kv);
    keys.push_back(kv.key);

    if ((i + 1) % batch_size == 0 || (i + 1 == FLAGS_arrange_kv_num)) {
      auto result = KvTxnBatchPut(kvs);
      if (!result.status.ok()) {
        return false;
      }
      kvs.clear();
      std::cout << '\r'
                << fmt::format("region({}) put data({}) progress [{}%]", FLAGS_arrange_kv_num, prefix,
                               i * 100 / FLAGS_arrange_kv_num)
                << std::flush;
    }
  }

  std::cout << "\r" << fmt::format("region({}) put data({}) ............ done", prefix, FLAGS_arrange_kv_num) << '\n';

  return true;
}

Operation::Result TxnReadSeqOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    return KvTxnGet({keys[region_entry->read_index++ % keys.size()]});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      batch_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadSeqOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      tmp_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        batch_keys.push_back(keys[region_entry->read_index++ % keys.size()]);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

Operation::Result TxnReadRandomOperation::Execute(RegionEntryPtr region_entry) {
  auto& keys = region_entry->keys;

  if (FLAGS_batch_size <= 1) {
    uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
    return KvTxnGet({keys[index]});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      batch_keys.push_back(keys[index]);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadRandomOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
      tmp_keys.push_back(keys[index]);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        uint32_t index = dingodb::benchmark::GenerateRealRandomInteger(0, UINT32_MAX) % keys.size();
        batch_keys.push_back(keys[index]);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

Operation::Result TxnReadMissingOperation::Execute(RegionEntryPtr region_entry) {
  auto& prefix = region_entry->prefix;

  int random_str_len = FLAGS_key_size + 4 - prefix.size();
  if (FLAGS_batch_size <= 1) {
    std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
    return KvTxnGet({key});
  } else {
    std::vector<std::string> batch_keys;
    batch_keys.reserve(FLAGS_batch_size);
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      batch_keys.push_back(key);
    }
    return KvTxnBatchGet({batch_keys});
  }
}

Operation::Result TxnReadMissingOperation::Execute(std::vector<RegionEntryPtr>& region_entries) {
  if (FLAGS_batch_size <= 1) {
    std::vector<std::string> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& prefix = region_entry->prefix;
      int random_str_len = FLAGS_key_size + 4 - prefix.size();

      std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
      tmp_keys.push_back(key);
    }

    return KvTxnGet(tmp_keys);
  } else {
    std::vector<std::vector<std::string>> tmp_keys;
    for (auto& region_entry : region_entries) {
      auto& keys = region_entry->keys;
      auto& prefix = region_entry->prefix;
      int random_str_len = FLAGS_key_size + 4 - prefix.size();

      std::vector<std::string> batch_keys;
      batch_keys.reserve(FLAGS_batch_size);
      for (int i = 0; i < FLAGS_batch_size; ++i) {
        std::string key = EncodeRawKey(prefix + GenRandomString(random_str_len));
        batch_keys.push_back(key);
      }

      tmp_keys.push_back(batch_keys);
    }

    return KvTxnBatchGet(tmp_keys);
  }
}

template <typename T>
static void PrintVector(const std::vector<T>& vec) {
  for (const auto& v : vec) {
    std::cout << v << " ";
  }

  std::cout << '\n';
}

static std::vector<float> FakeGenerateFloatVector(uint32_t dimension) {
  std::vector<float> vec;
  vec.reserve(dimension);
  for (int i = 0; i < dimension; ++i) {
    vec.push_back(0.12313 * (i + 1));
  }

  return vec;
}

sdk::VectorWithId GenVectorWithId(int64_t vector_id) {
  sdk::VectorWithId vector_with_id;

  vector_with_id.id = vector_id;
  vector_with_id.vector.dimension = FLAGS_vector_dimension;

  if (FLAGS_vector_value_type == "FLOAT") {
    vector_with_id.vector.value_type = sdk::ValueType::kFloat;
    vector_with_id.vector.float_values = dingodb::benchmark::GenerateFloatVector(FLAGS_vector_dimension);
    // vector_with_id.vector.float_values = FakeGenerateFloatVector(FLAGS_vector_dimension);
  } else {
    vector_with_id.vector.value_type = sdk::ValueType::kUint8;
    vector_with_id.vector.binary_values = dingodb::benchmark::GenerateInt8Vector(FLAGS_vector_dimension);
  }

  return vector_with_id;
}

Operation::Result VectorFillSeqOperation::Execute(VectorIndexEntryPtr entry) {
  std::vector<sdk::VectorWithId> vector_with_ids;
  vector_with_ids.reserve(FLAGS_batch_size);
  if (FLAGS_batch_size <= 1) {
    vector_with_ids.push_back(GenVectorWithId(entry->GenId()));

  } else {
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      vector_with_ids.push_back(GenVectorWithId(entry->GenId()));
    }
  }

  return VectorPut(entry, vector_with_ids);
}

Operation::Result VectorFillRandomOperation::Execute(VectorIndexEntryPtr entry) {
  std::vector<sdk::VectorWithId> vector_with_ids;
  vector_with_ids.reserve(FLAGS_batch_size);
  if (FLAGS_batch_size <= 1) {
    int64_t vector_id = dingodb::benchmark::GenerateRealRandomInteger(1, 100000000);
    vector_with_ids.push_back(GenVectorWithId(vector_id));

  } else {
    std::set<int64_t> vector_id_set;
    for (int i = 0; i < FLAGS_batch_size;) {
      int64_t vector_id = dingodb::benchmark::GenerateRealRandomInteger(1, 100000000);
      if (vector_id_set.count(vector_id) == 0) {
        vector_with_ids.push_back(GenVectorWithId(vector_id));
        vector_id_set.insert(vector_id);
        ++i;
      }
    }
  }

  return VectorPut(entry, vector_with_ids);
}

bool VectorSearchOperation::Arrange(VectorIndexEntryPtr entry, DatasetPtr dataset) {
  if (!FLAGS_vector_search_arrange_data) {
    if (!FLAGS_vector_dataset.empty()) {
      entry->test_entries = dataset->GetTestData();
      return !entry->test_entries.empty();
    }
  } else {
    if (FLAGS_vector_dataset.empty()) {
      return ArrangeAutoData(entry);
    } else {
      bool ret = ArrangeManualData(entry, dataset);
      if (!ret) {
        return false;
      }
      entry->test_entries = dataset->GetTestData();
      return !entry->test_entries.empty();
    }
  }

  return true;
}

bool VectorSearchOperation::ArrangeAutoData(VectorIndexEntryPtr entry) {
  std::vector<std::thread> threads;
  threads.reserve(FLAGS_vector_arrange_concurrency);

  uint32_t put_count_per_thread = FLAGS_arrange_kv_num / FLAGS_vector_arrange_concurrency;

  std::atomic<int> stop_count = 0;
  std::atomic<uint32_t> count = 0;
  std::atomic<uint64_t> fail_count = 0;

  for (int thread_no = 0; thread_no < FLAGS_vector_arrange_concurrency; ++thread_no) {
    uint32_t actual_put_count_per_thread =
        thread_no == 0 ? put_count_per_thread + (FLAGS_arrange_kv_num % FLAGS_vector_arrange_concurrency)
                       : put_count_per_thread;
    threads.emplace_back([this, thread_no, entry, actual_put_count_per_thread, &stop_count, &count, &fail_count]() {
      std::vector<sdk::VectorWithId> vector_with_ids;
      vector_with_ids.reserve(FLAGS_vector_put_batch_size);

      bool retry = false;
      for (uint32_t i = 1; i <= actual_put_count_per_thread;) {
        if (!retry) {
          vector_with_ids.push_back(GenVectorWithId(entry->GenId()));
        }

        if (vector_with_ids.size() == FLAGS_vector_put_batch_size || i == actual_put_count_per_thread) {
          auto result = VectorPut(entry, vector_with_ids);
          if (!result.status.IsOK()) {
            fail_count.fetch_add(vector_with_ids.size());
            retry = true;
            continue;
          }

          count.fetch_add(vector_with_ids.size());
          vector_with_ids.clear();
          retry = false;
        }

        ++i;
      }

      stop_count.fetch_add(1);
    });
  }

  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (stop_count.load() == FLAGS_vector_arrange_concurrency) {
      break;
    }

    uint32_t cur_count = count.load();
    std::cout << '\r'
              << fmt::format("vector index({}) put data progress [{} / {} / {} {}%]", entry->index_id, cur_count,
                             fail_count.load(), FLAGS_arrange_kv_num, cur_count * 100 / FLAGS_arrange_kv_num)
              << std::flush;
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::cout << "\r"
            << fmt::format("vector index({}) put data success({}) fail({}) .................. done", entry->index_id,
                           count.load(), fail_count.load())
            << '\n';

  return true;
}

// parallel put
bool VectorSearchOperation::ArrangeManualData(VectorIndexEntryPtr entry, DatasetPtr dataset) {
  uint32_t total_vector_count = dataset->GetTrainDataCount();

  std::vector<std::thread> threads;
  threads.reserve(FLAGS_vector_arrange_concurrency);

  std::atomic<int> stop_count = 0;
  std::atomic<uint32_t> count = 0;
  std::atomic<uint64_t> fail_count = 0;
  for (int thread_no = 0; thread_no < FLAGS_vector_arrange_concurrency; ++thread_no) {
    threads.emplace_back([this, thread_no, entry, dataset, &stop_count, &count, &fail_count]() {
      std::vector<sdk::VectorWithId> vector_with_ids;
      bool retry = false;
      bool is_eof = false;
      for (int batch_num = thread_no;;) {
        if (!retry) {
          uint64_t start_time = dingodb::benchmark::TimestampUs();
          dataset->GetBatchTrainData(batch_num, vector_with_ids, is_eof);
        }
        if (!vector_with_ids.empty()) {
          uint64_t start_time = dingodb::benchmark::TimestampUs();
          auto result = VectorPut(entry, vector_with_ids);
          LOG(INFO) << fmt::format("put vector size({}) elapsed time: {}us", vector_with_ids.size(),
                                   dingodb::benchmark::TimestampUs() - start_time);
          if (!result.status.IsOK()) {
            fail_count.fetch_add(vector_with_ids.size());
            retry = true;
            continue;
          }

          count.fetch_add(vector_with_ids.size());
          vector_with_ids.clear();
          batch_num += FLAGS_vector_arrange_concurrency;
          retry = false;
        }

        if (is_eof) {
          break;
        }
      }

      stop_count.fetch_add(1);
    });
  }

  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (stop_count.load() == FLAGS_vector_arrange_concurrency) {
      break;
    }

    uint32_t cur_count = count.load();
    if (total_vector_count > 0) {
      std::cout << '\r'
                << fmt::format("Vector index({}) put data progress [{} / {} / {} {}%]", entry->index_id, cur_count,
                               fail_count.load(), total_vector_count, cur_count * 100 / total_vector_count)
                << std::flush;
    } else {
      std::cout << '\r'
                << fmt::format("Vector index({}) put data progress [{} / {}]", entry->index_id, cur_count,
                               fail_count.load())
                << std::flush;
    }
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::cout << "\r"
            << fmt::format("Vector index({}) put data success({}) fail({}) .................. done", entry->index_id,
                           count.load(), fail_count.load())
            << '\n';

  return true;
}

Operation::Result VectorSearchOperation::Execute(VectorIndexEntryPtr entry) {
  return FLAGS_vector_dataset.empty() ? ExecuteAutoData(entry) : ExecuteManualData(entry);
}

Operation::Result VectorSearchOperation::ExecuteAutoData(VectorIndexEntryPtr entry) {
  std::vector<sdk::VectorWithId> vector_with_ids;
  vector_with_ids.reserve(FLAGS_batch_size);

  sdk::SearchParam search_param;
  search_param.with_vector_data = FLAGS_with_vector_data;
  search_param.with_scalar_data = FLAGS_with_scalar_data;
  search_param.with_table_data = FLAGS_with_table_data;
  search_param.use_brute_force = FLAGS_vector_search_use_brute_force;

  if (FLAGS_vector_search_enable_range_search) {
    search_param.radius = FLAGS_vector_search_radius;
  } else {
    search_param.topk = FLAGS_vector_search_topk;
  }

  std::string filter_type = dingodb::benchmark::ToUpper(FLAGS_vector_search_filter_type);
  if (filter_type == "PRE") {
    search_param.filter_type = sdk::FilterType::kQueryPre;
  } else if (filter_type == "POST") {
    search_param.filter_type = sdk::FilterType::kQueryPost;
  } else {
    search_param.filter_type = sdk::FilterType::kNoneFilterType;
  }

  std::string filter_source = dingodb::benchmark::ToUpper(FLAGS_vector_search_filter_source);
  if (filter_source == "SCALAR") {
    search_param.filter_source = sdk::FilterSource::kScalarFilter;
  } else if (filter_source == "TABLE") {
    search_param.filter_source = sdk::FilterSource::kTableFilter;
  } else if (filter_source == "VECTOR_ID") {
    search_param.filter_source = sdk::FilterSource::kVectorIdFilter;
  } else {
    search_param.filter_source = sdk::FilterSource::kNoneFilterSource;
  }

  // vector id filter
  if (search_param.filter_source == sdk::FilterSource::kVectorIdFilter) {
    for (int i = 0; i < FLAGS_vector_search_filter_vector_id_num; ++i) {
      search_param.vector_ids.push_back(dingodb::benchmark::GenerateRealRandomInteger(1, 10000000));
      search_param.is_negation = FLAGS_filter_vector_id_is_negation;
      search_param.is_sorted = true;
    }
  }

  if (FLAGS_batch_size <= 1) {
    vector_with_ids.push_back(GenVectorWithId(0));
  } else {
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      vector_with_ids.push_back(GenVectorWithId(0));
    }
  }

  return VectorSearch(entry, vector_with_ids, search_param);
}

uint32_t CalculateRecallRate(const std::unordered_map<int64_t, float>& neighbors,
                             const std::vector<sdk::VectorWithDistance>& vector_with_distances) {
  uint32_t hit_count = 0;
  for (const auto& vector_with_distance : vector_with_distances) {
    if (neighbors.find(vector_with_distance.vector_data.id) != neighbors.end()) {
      ++hit_count;
    }
  }

  return (hit_count * 10000) / neighbors.size();
}

Operation::Result VectorSearchOperation::ExecuteManualData(VectorIndexEntryPtr entry) {
  if (FLAGS_vector_index_type == "DISKANN") {
    if (!already_prepare_for_diskann_) {
      std::lock_guard lock(mutex_);
      if (!already_prepare_for_diskann_) {
        auto success = PrepareForDiskANNBeforeVectorSearch(entry);
        if (!success) {
          DINGO_LOG(ERROR) << "PrepareForDiskANNBeforeVectorSearch failed";
          exit(-1);
        }
        already_prepare_for_diskann_ = true;
      } // if (!already_prepare_for_diskann_) {
    }  // if (!already_prepare_for_diskann_) {
  }

  std::vector<sdk::VectorWithId> vector_with_ids;
  vector_with_ids.reserve(FLAGS_batch_size);

  sdk::SearchParam search_param;
  search_param.with_vector_data = FLAGS_with_vector_data;
  search_param.with_scalar_data = FLAGS_with_scalar_data;
  search_param.with_table_data = FLAGS_with_table_data;
  search_param.use_brute_force = FLAGS_vector_search_use_brute_force;
  search_param.topk = FLAGS_vector_search_topk;

  if (FLAGS_vector_index_type == "IVF_FLAT" || FLAGS_vector_index_type == "IVF_PQ") {
    search_param.extra_params.insert(std::make_pair(sdk::SearchExtraParamType::kNprobe, FLAGS_vector_search_nprobe));
  }

  if (FLAGS_vector_index_type == "HNSW") {
    search_param.extra_params.insert(std::make_pair(sdk::SearchExtraParamType::kEfSearch, FLAGS_vector_search_ef));
  }

  if (FLAGS_vector_index_type == "DISKANN") {
    search_param.beamwidth = FLAGS_diskann_search_beamwidth;
  }

  std::string filter_type = dingodb::benchmark::ToUpper(FLAGS_vector_search_filter_type);
  if (filter_type == "PRE") {
    search_param.filter_type = sdk::FilterType::kQueryPre;
  } else if (filter_type == "POST") {
    search_param.filter_type = sdk::FilterType::kQueryPost;
  } else {
    search_param.filter_type = sdk::FilterType::kNoneFilterType;
  }

  std::string filter_source = dingodb::benchmark::ToUpper(FLAGS_vector_search_filter_source);
  if (filter_source == "SCALAR") {
    search_param.filter_source = sdk::FilterSource::kScalarFilter;
  } else if (filter_source == "TABLE") {
    search_param.filter_source = sdk::FilterSource::kTableFilter;
  } else if (filter_source == "VECTOR_ID") {
    search_param.filter_source = sdk::FilterSource::kVectorIdFilter;
  } else {
    search_param.filter_source = sdk::FilterSource::kNoneFilterSource;
  }

  auto offset = entry->GenId();
  auto& all_test_entries = entry->test_entries;

  // scalar filter
  if (FLAGS_use_coprocessor && !all_test_entries[0]->filter_json.empty()) {
    search_param.langchain_expr_json = all_test_entries[0]->filter_json;
    DINGO_LOG(INFO) << "langchain_expr_json: " << search_param.langchain_expr_json;
  }

  std::vector<Dataset::TestEntryPtr> batch_test_entries;
  if (FLAGS_batch_size <= 1) {
    auto& test_entry = all_test_entries[offset % all_test_entries.size()];
    if (search_param.filter_type == sdk::FilterType::kNoneFilterType &&
        search_param.filter_source == sdk::FilterSource::kNoneFilterSource) {
      sdk::VectorWithId vector_with_id;
      vector_with_id.id = test_entry->vector_with_id.id;
      vector_with_id.vector = test_entry->vector_with_id.vector;
      vector_with_ids.push_back(vector_with_id);
    } else {
      vector_with_ids.push_back(test_entry->vector_with_id);
    }
    batch_test_entries.push_back(test_entry);

    // vector id filter
    if (search_param.filter_source == sdk::FilterSource::kVectorIdFilter && !test_entry->filter_vector_ids.empty()) {
      search_param.vector_ids = test_entry->filter_vector_ids;
      search_param.is_negation = FLAGS_filter_vector_id_is_negation;
      search_param.is_sorted = true;
      DINGO_LOG(INFO) << fmt::format("search_param.vector_ids size: {}", search_param.vector_ids.size());
    }

  } else {
    for (size_t i = offset; i < FLAGS_batch_size; ++i) {
      auto& test_entry = all_test_entries[i % all_test_entries.size()];
      if (search_param.filter_type == sdk::FilterType::kNoneFilterType &&
          search_param.filter_source == sdk::FilterSource::kNoneFilterSource) {
        sdk::VectorWithId vector_with_id;
        vector_with_id.id = test_entry->vector_with_id.id;
        vector_with_id.vector = test_entry->vector_with_id.vector;
        vector_with_ids.push_back(vector_with_id);
      } else {
        vector_with_ids.push_back(test_entry->vector_with_id);
      }
      batch_test_entries.push_back(test_entry);
    }

    // vector id filter, use first entry.
    auto& test_entry = all_test_entries[offset % all_test_entries.size()];
    if (search_param.filter_source == sdk::FilterSource::kVectorIdFilter && !test_entry->filter_vector_ids.empty()) {
      search_param.vector_ids = test_entry->filter_vector_ids;
      search_param.is_negation = FLAGS_filter_vector_id_is_negation;
      search_param.is_sorted = true;
    }
  }
  ready_report.store(true);
  auto result = VectorSearch(entry, vector_with_ids, search_param);
  if (!result.status.IsOK()) {
    return result;
  }

  for (uint32_t i = 0; i < batch_test_entries.size(); ++i) {
    auto& entry = batch_test_entries[i];
    if (i < result.vector_search_results.size()) {
      auto& search_result = result.vector_search_results[i];

      result.recalls.push_back(CalculateRecallRate(entry->neighbors, search_result.vector_datas));
    } else {
      result.recalls.push_back(0);
    }
  }

  return result;
}

// for diskann
bool VectorSearchOperation::PrepareForDiskANNBeforeVectorSearch(VectorIndexEntryPtr entry) {
  Operation::Result result;
  sdk::Status status;
  sdk::StateResult state_result;
  std::vector<int64_t> region_ids;

  std::string region_id_info = "region ids : ";

  auto dump_region_ids = [&region_ids]() {
    std::string str;
    for (const auto& region_id : region_ids) {
      str += std::to_string(region_id) + " ";
    }
    return str;
  };

  auto beautiful_display_time = [](int64_t start_time_second, int64_t end_time_second) {
    int64_t cost_time = end_time_second - start_time_second;
    int64_t hour = cost_time / 3600;
    int64_t minute = (cost_time % 3600) / 60;
    int64_t second = cost_time % 60;

    if (hour > 0) {
      return fmt::format("{} h {} m {} s", hour, minute, second);
    }

    if (minute > 0) {
      return fmt::format("{} m {} s", minute, second);
    }

    return fmt::format("{} s", second);
  };

  sdk::VectorClient* vector_client = nullptr;
  result.status = client->NewVectorClient(&vector_client);
  if (!result.status.IsOK()) {
    DINGO_LOG(ERROR) << fmt::format("new NewVectorClient failed");
    return false;
  }

  std::shared_ptr<sdk::VectorClient> vector_client_ptr(vector_client);
  vector_client = nullptr;

  result.status = status = vector_client_ptr->StatusByIndexId(entry->index_id, state_result);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << fmt::format("StatusByIndexId failed : {}", status.ToString());
    return false;
  }

  region_ids.reserve(state_result.region_states.size());
  for (auto& result : state_result.region_states) {
    region_ids.push_back(result.region_id);
  }

  region_id_info += dump_region_ids();
  std::cout << region_id_info << this << std::endl;
  DINGO_LOG(INFO) << region_id_info << this;

  // build
  sdk::ErrStatusResult err_status_result;
  status = vector_client_ptr->BuildByRegionId(entry->index_id, region_ids, err_status_result);
  if (status.IsBuildFailed()) {
    DINGO_LOG(ERROR) << "vector build failed : {} " << status.ToString();
    return false;
  }

  // wait regions builded
  std::vector<int64_t> build_region_ids = region_ids;
  std::string building_str = "building ";
  int64_t start_time = dingodb::benchmark::Timestamp();
  while (true) {
    state_result.region_states.clear();
    status = vector_client_ptr->StatusByRegionId(entry->index_id, build_region_ids, state_result);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("StatusByRegionId failed : {}", status.ToString());
      return false;
    }

    for (const auto& state : state_result.region_states) {
      if (state.state == sdk::DiskANNRegionState::kBuilding) {
        continue;
      }

      if (state.state == sdk::DiskANNRegionState::kBuildFailed) {
        DINGO_LOG(ERROR) << fmt::format("region({}) build failed", state.region_id);
        return false;
      }

      if (state.state == sdk::DiskANNRegionState::kBuilded || state.state == sdk::DiskANNRegionState::kNoData) {
        auto it = std::find(build_region_ids.begin(), build_region_ids.end(), state.region_id);
        if (it != build_region_ids.end()) {
          build_region_ids.erase(it);
        }
      }
    }

    if (build_region_ids.empty()) {
      break;
    }

    building_str += '.';
    std::cout << '\r' << building_str << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  int64_t end_time = dingodb::benchmark::Timestamp();
  DINGO_LOG(INFO) << building_str;
  std::cout << "\nbuild cost : " << beautiful_display_time(start_time, end_time) << std::endl;
  DINGO_LOG(INFO) << "\nbuild cost : " << beautiful_display_time(start_time, end_time);

  // load
  status = vector_client_ptr->LoadByIndexId(entry->index_id, err_status_result);
  if (status.IsLoadFailed()) {
    DINGO_LOG(ERROR) << fmt::format("vector load failed : {}", status.ToString());
    return false;
  }

  // wait regions loaded
  std::vector<int64_t> load_region_ids = region_ids;
  std::string loading_str = "loading ";
  start_time = dingodb::benchmark::Timestamp();
  while (true) {
    state_result.region_states.clear();
    status = vector_client_ptr->StatusByRegionId(entry->index_id, load_region_ids, state_result);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("StatusByRegionId failed : {}", status.ToString());
      return false;
    }

    for (const auto& state : state_result.region_states) {
      if (state.state == sdk::DiskANNRegionState::kLoading) {
        continue;
      }

      if (state.state == sdk::DiskANNRegionState::kLoadFailed) {
        DINGO_LOG(ERROR) << fmt::format("region({}) load failed", state.region_id);
        return false;
      }

      if (state.state == sdk::DiskANNRegionState::kLoaded || state.state == sdk::DiskANNRegionState::kNoData) {
        auto it = std::find(load_region_ids.begin(), load_region_ids.end(), state.region_id);
        if (it != load_region_ids.end()) {
          load_region_ids.erase(it);
        }
      }
    }

    if (load_region_ids.empty()) {
      break;
    }

    loading_str += '.';
    std::cout << '\r' << loading_str << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  end_time = dingodb::benchmark::Timestamp();
  DINGO_LOG(INFO) << loading_str;
  std::cout << "\nload cost : " << beautiful_display_time(start_time, end_time) << std::endl;
  DINGO_LOG(INFO) << "\nload cost : " << beautiful_display_time(start_time, end_time);

  return true;
}

Operation::Result VectorQueryOperation::Execute(VectorIndexEntryPtr entry) {
  return FLAGS_vector_dataset.empty() ? ExecuteAutoData(entry) : ExecuteManualData(entry);
}

Operation::Result VectorQueryOperation::ExecuteAutoData(VectorIndexEntryPtr entry) {
  std::vector<int64_t> vector_ids;
  vector_ids.reserve(FLAGS_batch_size);

  sdk::QueryParam query_param;
  query_param.with_vector_data = FLAGS_with_vector_data;
  query_param.with_scalar_data = FLAGS_with_scalar_data;
  query_param.with_table_data = FLAGS_with_table_data;

  if (FLAGS_batch_size <= 1) {
    vector_ids.push_back(entry->GenId());
  } else {
    for (int i = 0; i < FLAGS_batch_size; ++i) {
      vector_ids.push_back(entry->GenId());
    }
  }

  query_param.vector_ids = vector_ids;

  return VectorBatchQuery(entry, query_param);
}

Operation::Result VectorQueryOperation::ExecuteManualData(VectorIndexEntryPtr entry) {
  std::vector<int64_t> vector_ids;
  vector_ids.reserve(FLAGS_batch_size);

  sdk::QueryParam query_param;
  query_param.with_vector_data = FLAGS_with_vector_data;
  query_param.with_scalar_data = FLAGS_with_scalar_data;
  query_param.with_table_data = FLAGS_with_table_data;

  auto offset = entry->GenId();
  auto& all_test_entries = entry->test_entries;

  if (FLAGS_batch_size <= 1) {
    auto& test_entry = all_test_entries[offset % all_test_entries.size()];
    vector_ids.push_back(test_entry->vector_with_id.id);
  } else {
    for (size_t i = offset; i < FLAGS_batch_size; ++i) {
      auto& test_entry = all_test_entries[i % all_test_entries.size()];
      vector_ids.push_back(test_entry->vector_with_id.id);
    }
  }

  query_param.vector_ids = vector_ids;

  return VectorBatchQuery(entry, query_param);
}

bool IsSupportBenchmarkType(const std::string& benchmark) {
  auto it = support_operations.find(benchmark);
  return it != support_operations.end();
}

std::string GetSupportBenchmarkType() {
  std::string benchmarks;
  for (auto& [benchmark, _] : support_operations) {
    benchmarks += benchmark;
    benchmarks += " ";
  }

  return benchmarks;
}

OperationPtr NewOperation(std::shared_ptr<sdk::Client> client) {
  auto it = support_operations.find(FLAGS_benchmark);
  if (it == support_operations.end()) {
    return nullptr;
  }

  return it->second(client);
}

}  // namespace benchmark
}  // namespace dingodb
